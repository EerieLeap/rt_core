cmake_minimum_required(VERSION 3.20.0)

set(RT_CORE_TEST_SIM_FLASH ON)
# Two CAN controllers on every board: can0, which the stored configuration runs, and can1, which it doesn't.
list(APPEND EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/can_monitor.overlay")
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../rt_core_test.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(functional_can_monitor_domain)

file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.cpp)

target_sources(app PRIVATE ${TEST_SOURCES})

# The test client injects requests with smp_rx_req().
target_include_directories(app PRIVATE
  ${RT_CORE_DIR}/tests/common
  ${ZEPHYR_BASE}/subsys/mgmt/mcumgr/transport/include
)
