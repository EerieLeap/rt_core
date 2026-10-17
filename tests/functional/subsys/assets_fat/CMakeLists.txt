cmake_minimum_required(VERSION 3.20.0)

set(RT_CORE_TEST_FLASH_DISK ON)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../rt_core_test.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(functional_subsys_assets_fat)

# The store tests of the assets suite, on a FAT volume instead of littlefs. The registry tests do not
# depend on the volume and run there only.
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../assets/src/asset_store.cpp)

# The block tests in that file record the calls the store makes into the file system through wrappers
# of these.
zephyr_link_libraries(-Wl,--wrap=fs_read,--wrap=fs_write,--wrap=fs_seek)
