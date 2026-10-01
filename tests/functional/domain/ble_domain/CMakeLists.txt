cmake_minimum_required(VERSION 3.20.0)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../rt_core_test.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(functional_ble_domain)

file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.cpp)

# The BLE domain needs Bluetooth, so its live data parts are built on their own here.
target_sources(app PRIVATE
  ${RT_CORE_DIR}/src/domain/ble_domain/utilities/live_data_encoder.cpp
  ${RT_CORE_DIR}/src/domain/ble_domain/services/live_data_service.cpp
  ${RT_CORE_DIR}/src/domain/ble_domain/smp/live_mgmt_group.cpp
  ${TEST_SOURCES}
)

# The test client injects requests with smp_rx_req().
target_include_directories(app PRIVATE
  ${RT_CORE_DIR}/tests/common
  ${ZEPHYR_BASE}/subsys/mgmt/mcumgr/transport/include
)
