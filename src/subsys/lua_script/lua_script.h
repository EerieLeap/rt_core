#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include <zephyr/kernel.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "subsys/threading/scoped_mutex.h"

namespace eerie_leap::subsys::lua_script {

using eerie_leap::subsys::threading::ScopedMutex;

// One Lua state that hosts many scripts.
//
// Each script loaded with LoadChunk() runs in its own environment table whose metatable indexes
// the globals, so scripts see the standard libraries and the registered functions but not each
// other's globals. Functions are resolved once with FindFunction() and called through their
// registry reference. A state is not thread-safe: callers that share one take Lock() around every
// use of the state, including LoadChunk() and the calls through a reference.
//
// Memory for a state created with CreateExt() comes from the external heap and is counted; an
// allocation that would exceed the limit fails, which Lua reports as a memory error from the call
// that needed it, so one script cannot exhaust the heap for the others.
class LuaScript {
public:
    static constexpr int kNoRef = LUA_NOREF;

    struct MemoryBudget {
        size_t used = 0;
        size_t peak = 0;
        size_t limit = 0;   // 0 = unlimited
    };

private:
    // Heap-allocated so the state's user data and the mutex keep their addresses across moves.
    struct Context {
        k_mutex mutex;
        MemoryBudget budget;
    };

    lua_State* state_;
    std::unique_ptr<Context> context_;

    static int sleep_ms_func(lua_State* state);
    static int print_func(lua_State* state);

    static const luaL_Reg lua_std_libs_[];
    static const luaL_Reg static_global_functions_[];

    LuaScript(lua_State* state, std::unique_ptr<Context> context);

    void OpenLuaStdLibs(lua_State* L);
    static void* ExtMemoryAllocator(void* ud, void* ptr, size_t osize, size_t nsize);

public:
    ~LuaScript();

    LuaScript(const LuaScript&) = delete;
    LuaScript& operator=(const LuaScript&) = delete;
    LuaScript(LuaScript&& other) noexcept
        : state_(other.state_), context_(std::move(other.context_)) {
        other.state_ = nullptr;
    }

    // Create() uses the default allocator and has no memory limit. CreateExt() uses the external
    // heap with the configured limit (CONFIG_EERIE_LEAP_LUA_SCRIPT_MEMORY_LIMIT_KB).
    static LuaScript Create();
    static LuaScript CreateExt();

    bool IsValid() const { return state_ != nullptr; }
    lua_State* GetState() { return state_; }

    [[nodiscard]] ScopedMutex Lock() { return ScopedMutex(context_->mutex); }

    // Runs @p script in the global environment. For a state that hosts a single script.
    void Load(const std::span<const uint8_t>& script);

    // Compiles and runs @p script in a fresh environment and returns a reference to that
    // environment, or kNoRef when it fails to compile or run (logged with @p name).
    int LoadChunk(std::span<const uint8_t> script, const char* name);

    // A reference to the function @p name of the environment @p environment_ref, or kNoRef.
    int FindFunction(int environment_ref, const char* name);

    // Pushes the referenced value for a call; the caller handles the stack from there.
    void PushRef(int ref) { lua_rawgeti(state_, LUA_REGISTRYINDEX, ref); }
    void Release(int& ref);

    void RegisterGlobalFunction(const std::string& name, lua_CFunction func, void* object = nullptr);

    // One incremental collection step, to be called after a script call so collection work is
    // spread over calls instead of landing on one sample.
    void CollectStep();

    void SetMemoryLimit(size_t bytes) { context_->budget.limit = bytes; }
    const MemoryBudget& GetMemoryBudget() const { return context_->budget; }

    // Bytes the state holds; the counter for a CreateExt() state, Lua's own figure otherwise.
    size_t GetMemoryUsed() const;
    void LogMemoryUsage() const;
};

} // namespace eerie_leap::subsys::lua_script
