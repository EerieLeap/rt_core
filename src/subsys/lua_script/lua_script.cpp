#include <algorithm>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "utilities/memory/heap_allocator.h"

#include "lua_script.h"

namespace eerie_leap::subsys::lua_script {

using namespace eerie_leap::utilities::memory;

LOG_MODULE_REGISTER(lua);

#ifdef CONFIG_EERIE_LEAP_LUA_SCRIPT_MEMORY_LIMIT_KB
static constexpr size_t K_DEFAULT_MEMORY_LIMIT = static_cast<size_t>(CONFIG_EERIE_LEAP_LUA_SCRIPT_MEMORY_LIMIT_KB)
    * 1024U;
#else
static constexpr size_t K_DEFAULT_MEMORY_LIMIT = 0;
#endif

LuaScript::LuaScript(lua_State* state, std::unique_ptr<Context> context) :
    state_(state),
    context_(std::move(context)) {

    if(state_ == nullptr) {
        LOG_ERR("Failed to create Lua state.");
        return;
    }

    OpenLuaStdLibs(state_);
    lua_pushglobaltable(state_);
    luaL_setfuncs(state_, STATIC_GLOBAL_FUNCTIONS, 0);
    lua_pop(state_, 1);
}

LuaScript::~LuaScript() {
    if(state_ != nullptr)
        lua_close(state_);
}

LuaScript LuaScript::Create() {
    auto context = std::make_unique<Context>();
    k_mutex_init(&context->mutex);

    return { luaL_newstate(), std::move(context) };
}

LuaScript LuaScript::CreateExt() {
    auto context = std::make_unique<Context>();
    k_mutex_init(&context->mutex);
    context->budget.limit = K_DEFAULT_MEMORY_LIMIT;

    // The context is the allocator's user data, so the counter lives next to the limit.
    lua_State* state = lua_newstate(ExtMemoryAllocator, context.get());

    return { state, std::move(context) };
}

void LuaScript::Load(const std::span<const uint8_t>& script) {
    if(state_ == nullptr) {
        LOG_ERR("Cannot load script: Lua state is null");
        return;
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): luaL_loadbuffer takes the script bytes as char*
    if(luaL_loadbuffer(state_, reinterpret_cast<const char*>(script.data()), script.size(), "script") != LUA_OK) {
        LOG_ERR("Error loading script: %s", lua_tostring(state_, -1));
        lua_pop(state_, 1);
        return;
    }

    if(lua_pcall(state_, 0, 0, 0) != LUA_OK) {
        LOG_ERR("Error executing script: %s", lua_tostring(state_, -1));
        lua_pop(state_, 1);
        return;
    }
}

int LuaScript::LoadChunk(std::span<const uint8_t> script, const char* name) {
    if(state_ == nullptr) {
        LOG_ERR("Cannot load script %s: Lua state is null", name);
        return NO_REF;
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): luaL_loadbuffer takes the script bytes as char*
    if(luaL_loadbuffer(state_, reinterpret_cast<const char*>(script.data()), script.size(), name) != LUA_OK) {
        LOG_ERR("Error loading script %s: %s", name, lua_tostring(state_, -1));
        lua_pop(state_, 1);
        return NO_REF;
    }

    // A fresh environment that falls back to the globals for reads: the libraries and the
    // registered functions are visible, the script's own globals stay private to it.
    lua_newtable(state_);                                            // chunk, env
    lua_newtable(state_);                                            // chunk, env, mt
    lua_pushglobaltable(state_);                                     // chunk, env, mt, _G
    lua_setfield(state_, -2, "__index");                             // chunk, env, mt
    lua_setmetatable(state_, -2);                                    // chunk, env
    lua_pushvalue(state_, -1);                                       // chunk, env, env
    const int environment_ref = luaL_ref(state_, LUA_REGISTRYINDEX); // chunk, env
    lua_setupvalue(state_, -2, 1);                                   // chunk  (the chunk's _ENV is the first upvalue)

    if(lua_pcall(state_, 0, 0, 0) != LUA_OK) {
        LOG_ERR("Error executing script %s: %s", name, lua_tostring(state_, -1));
        lua_pop(state_, 1);
        luaL_unref(state_, LUA_REGISTRYINDEX, environment_ref);
        return NO_REF;
    }

    return environment_ref;
}

int LuaScript::FindFunction(int environment_ref, const char* name) {
    if(state_ == nullptr || environment_ref == NO_REF)
        return NO_REF;

    lua_rawgeti(state_, LUA_REGISTRYINDEX, environment_ref); // env
    lua_getfield(state_, -1, name);                          // env, value

    if(!lua_isfunction(state_, -1)) {
        lua_pop(state_, 2);
        return NO_REF;
    }

    const int function_ref = luaL_ref(state_, LUA_REGISTRYINDEX); // env
    lua_pop(state_, 1);

    return function_ref;
}

void LuaScript::Release(int& ref) {
    if(state_ != nullptr && ref != NO_REF)
        luaL_unref(state_, LUA_REGISTRYINDEX, ref);

    ref = NO_REF;
}

void LuaScript::RegisterGlobalFunction(const std::string& name, lua_CFunction func, void* object) {
    if(object != nullptr)
        lua_pushlightuserdata(state_, object);
    lua_pushcclosure(state_, func, object != nullptr ? 1 : 0);
    lua_setglobal(state_, name.c_str());
}

void LuaScript::CollectStep() {
    if(state_ != nullptr)
        lua_gc(state_, LUA_GCSTEP, 0);
}

size_t LuaScript::GetMemoryUsed() const {
    if(state_ == nullptr)
        return 0;

    if(context_->budget.used != 0)
        return context_->budget.used;

    return static_cast<size_t>(lua_gc(state_, LUA_GCCOUNT, 0)) * 1024U
        + static_cast<size_t>(lua_gc(state_, LUA_GCCOUNTB, 0));
}

void LuaScript::LogMemoryUsage() const {
    LOG_INF(
        "Lua state memory usage: %zu bytes (peak %zu, limit %zu)", GetMemoryUsed(), context_->budget.peak,
        context_->budget.limit
    );
}

void* LuaScript::ExtMemoryAllocator(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* context = static_cast<Context*>(ud);
    MemoryBudget& budget = context->budget;
    HeapAllocator<uint8_t> allocator;

    // Lua passes the object kind in osize when ptr is null, not a size.
    const size_t old_size = ptr != nullptr ? osize : 0;

    if(nsize == 0) {
        if(ptr != nullptr) {
            allocator.deallocate(static_cast<uint8_t*>(ptr), old_size);
            budget.used -= old_size;
        }

        return nullptr;
    }

    // Lua assumes a shrink never fails. The heap frees a block by address, not by size, so the
    // block is kept as it is and only the accounting follows the new size.
    if(ptr != nullptr && nsize <= old_size) {
        budget.used = budget.used - old_size + nsize;
        return ptr;
    }

    // Refused rather than served: Lua runs a full collection and retries once, then raises a
    // memory error in the running script.
    if(budget.limit != 0 && budget.used - old_size + nsize > budget.limit)
        return nullptr;

    void* new_ptr = nullptr;
    try {
        new_ptr = ptr == nullptr ? allocator.allocate(nsize)
                                 : allocator.reallocate(static_cast<uint8_t*>(ptr), old_size, nsize);
    } catch(const std::bad_alloc&) {
        new_ptr = nullptr;
    }

    if(new_ptr == nullptr) {
        LOG_ERR("Lua allocation failed: %zu bytes", nsize);
        return nullptr;
    }

    budget.used = budget.used - old_size + nsize;
    budget.peak = std::max(budget.peak, budget.used);

    return new_ptr;
}

int LuaScript::SleepMsFunc(lua_State* state) {
    const int n = lua_gettop(state);
    if(n != 1)
        return luaL_error(state, "expected 1 arguments, got %d", n);

    const int ms = luaL_checkinteger(state, 1);

    k_msleep(ms);

    return 0;
}

int LuaScript::PrintFunc(lua_State* state) {
    const int n = lua_gettop(state);
    if(n != 1)
        return luaL_error(state, "expected 1 arguments, got %d", n);

    const char* message = luaL_checkstring(state, 1);

    printk("%s\n", message);

    return 0;
}

void LuaScript::OpenLuaStdLibs(lua_State* l) {
    /* "require" functions from 'loadedlibs' and set results to global table */
    for(const luaL_Reg* lib = LUA_STD_LIBS; lib->func != nullptr; lib++) {
        luaL_requiref(l, lib->name, lib->func, 1);
        lua_pop(l, 1); /* remove lib */
    }
}

// Opened into every state. table, string, math and utf8 are always built; the base library
// follows the Lua module's Kconfig. The host's print and sleep_ms are set afterwards, so they
// replace the base library's print.
const luaL_Reg LuaScript::LUA_STD_LIBS[] = {
#ifdef CONFIG_LUA_LIB_BASE
    { LUA_GNAME, luaopen_base },
#endif
    // {LUA_LOADLIBNAME, luaopen_package},
    // {LUA_COLIBNAME, luaopen_coroutine},
    { LUA_TABLIBNAME, luaopen_table },
    // {LUA_IOLIBNAME, luaopen_io},
    // {LUA_OSLIBNAME, luaopen_os},
    { LUA_STRLIBNAME, luaopen_string }, { LUA_MATHLIBNAME, luaopen_math }, { LUA_UTF8LIBNAME, luaopen_utf8 },
    // {LUA_DBLIBNAME, luaopen_debug},
    { nullptr, nullptr } // Array end marker
};

const luaL_Reg LuaScript::STATIC_GLOBAL_FUNCTIONS[] = {
    { "sleep_ms", SleepMsFunc }, { "print", PrintFunc }, { nullptr, nullptr } // Array end marker
};

} // namespace eerie_leap::subsys::lua_script
