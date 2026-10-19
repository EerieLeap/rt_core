cmake_minimum_required(VERSION 3.20.0)

set(RT_CORE_TEST_SIM_FLASH ON)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../rt_core_test.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(functional_smp_groups)

file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.cpp)

# The scenario without the assets group (CONFIG_EERIE_LEAP_SMP_ASSETS=n) tests the rest of the suite
# as a unit without it runs.
if(NOT CONFIG_EERIE_LEAP_SMP_ASSETS)
  list(FILTER TEST_SOURCES EXCLUDE REGEX "/assets_mgmt_group\\.cpp$")
endif()

# The COM domain needs CAN and CDMP, and the logging group is built with the logging controller, so
# both groups are built on their own here.
target_sources(app PRIVATE
  ${RT_CORE_DIR}/src/domain/canbus_com_domain/smp/network_mgmt_group.cpp
  ${RT_CORE_DIR}/src/domain/logging_domain/smp/logging_mgmt_group.cpp
  ${TEST_SOURCES}
)

# The test client injects requests with smp_rx_req().
target_include_directories(app PRIVATE
  ${RT_CORE_DIR}/tests/common
  ${ZEPHYR_BASE}/subsys/mgmt/mcumgr/transport/include
)

# The assets group tests count the bytes the group reads from its files and the files it closes, and make
# a write fail, by wrappers of fs_read(), fs_close() and fs_write(), which are defined in the test of
# the group.
if(CONFIG_EERIE_LEAP_SMP_ASSETS)
  zephyr_link_libraries(-Wl,--wrap=fs_read,--wrap=fs_close,--wrap=fs_write)
endif()
