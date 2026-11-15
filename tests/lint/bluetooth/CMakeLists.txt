cmake_minimum_required(VERSION 3.20.0)

# The scenario that compiles the CAN monitor's controller (lint.bluetooth.can_monitor) passes this.
if(RT_CORE_LINT_CAN_MONITOR)
    list(APPEND EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/can_monitor.overlay")
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/../../rt_core_test.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lint_bluetooth)

target_sources(app PRIVATE src/main.cpp)
