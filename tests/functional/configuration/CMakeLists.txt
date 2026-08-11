cmake_minimum_required(VERSION 3.20.0)

set(RT_CORE_TEST_SIM_FLASH ON)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../rt_core_test.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(functional_configuration)

# CBOR configs are pulled in directly: the owning domains are not enabled here.
file(GLOB_RECURSE RT_CORE_CBOR_SOURCES
  ${RT_CORE_DIR}/src/configuration/cbor/traits/system_config_trait.cpp
  ${RT_CORE_DIR}/src/configuration/cbor/cbor_system_config/*.c
  ${RT_CORE_DIR}/src/configuration/cbor/traits/adc_config_trait.cpp
  ${RT_CORE_DIR}/src/configuration/cbor/cbor_adc_config/*.cpp
  ${RT_CORE_DIR}/src/configuration/cbor/traits/sensors_config_trait.cpp
  ${RT_CORE_DIR}/src/configuration/cbor/cbor_sensors_config/*.cpp
)

file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.cpp)

target_sources(app PRIVATE
  ${RT_CORE_CBOR_SOURCES}
  ${TEST_SOURCES}
)
