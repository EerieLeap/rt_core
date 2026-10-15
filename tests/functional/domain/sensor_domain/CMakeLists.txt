cmake_minimum_required(VERSION 3.20.0)

set(RT_CORE_TEST_SIM_FLASH ON)
set(RT_CORE_TEST_ADC_EMUL ON)
# The CAN controller the frame router listens on, on every board; boards/ holds the native_sim flash only.
list(APPEND EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/can_loopback.overlay")
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../rt_core_test.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(functional_sensor_domain)

file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.cpp)

target_sources(app PRIVATE ${TEST_SOURCES})

# Set by the raised_publisher_stack scenario (testcase.yaml): the throwing filter case must run there.
if(RT_CORE_TEST_RAISED_PUBLISHER_STACK)
    target_compile_definitions(app PRIVATE RT_CORE_TEST_RAISED_PUBLISHER_STACK)
endif()

# allocation_counter.h
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../common)
