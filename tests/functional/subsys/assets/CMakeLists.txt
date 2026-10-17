cmake_minimum_required(VERSION 3.20.0)

set(RT_CORE_TEST_SIM_FLASH ON)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../rt_core_test.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(functional_subsys_assets)

file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.cpp)

target_sources(app PRIVATE ${TEST_SOURCES})

# The block tests in src/asset_store.cpp record the calls the store makes into the file system through
# wrappers of these.
zephyr_link_libraries(-Wl,--wrap=fs_read,--wrap=fs_write,--wrap=fs_seek)
