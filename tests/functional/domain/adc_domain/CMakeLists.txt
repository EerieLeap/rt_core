cmake_minimum_required(VERSION 3.20.0)

set(RT_CORE_TEST_SIM_FLASH ON)
set(RT_CORE_TEST_ADC_EMUL ON)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../rt_core_test.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(functional_adc_domain)

file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.cpp)

target_sources(app PRIVATE ${TEST_SOURCES})

# With CONFIG_EERIE_LEAP_SMP the test client injects requests with smp_rx_req().
target_include_directories(app PRIVATE
  ${RT_CORE_DIR}/tests/common
  ${ZEPHYR_BASE}/subsys/mgmt/mcumgr/transport/include
)
