cmake_minimum_required(VERSION 3.20.0)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../rt_core_test.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(functional_smp_groups)

file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.cpp)

# The COM domain needs CAN and CDMP, so its group is built on its own here.
target_sources(app PRIVATE
  ${RT_CORE_DIR}/src/domain/canbus_com_domain/smp/network_mgmt_group.cpp
  ${TEST_SOURCES}
)

# The test client injects requests with smp_rx_req().
target_include_directories(app PRIVATE
  ${RT_CORE_DIR}/tests/common
  ${ZEPHYR_BASE}/subsys/mgmt/mcumgr/transport/include
)
