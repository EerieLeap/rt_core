cmake_minimum_required(VERSION 3.20.0)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../rt_core_test.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(unit_subsys_cdmp)

file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.cpp)

target_sources(app PRIVATE ${TEST_SOURCES})
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../common)

# The QEMU boards have no hwinfo driver, so hwinfo_get_device_id() answers -ENOSYS there and
# CdmpUid::Generate() never takes its hardware branch. A fixed device id stands in for one; the
# suite switches it off to reach the random fallback (cdmp_uid.cpp, under the same condition).
if(NOT CONFIG_HWINFO_HAS_DRIVER)
    target_sources(app PRIVATE fakes/hwinfo_device_id.cpp)
    target_include_directories(app PRIVATE fakes)
endif()
