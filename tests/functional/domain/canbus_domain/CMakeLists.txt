cmake_minimum_required(VERSION 3.20.0)

set(RT_CORE_TEST_SIM_FLASH ON)
# The CAN controller the scheduler sends on, on every board; boards/ holds the native_sim flash only.
list(APPEND EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/can_loopback.overlay")
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../rt_core_test.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(functional_canbus_domain)

file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.cpp)

target_sources(app PRIVATE ${TEST_SOURCES})
