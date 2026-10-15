cmake_minimum_required(VERSION 3.20.0)

set(RT_CORE_TEST_SIM_FLASH ON)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../rt_core_test.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(unit_domain)

file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.cpp)

target_sources(app PRIVATE ${TEST_SOURCES})

# Logging control builds with EERIE_LEAP_CONTROLLER_LOGGING, which needs the CDMP domain this suite
# leaves out, so its configuration code is compiled here, as functional/configuration does with the
# codecs.
target_sources(app PRIVATE
  ${RT_CORE_DIR}/src/domain/logging_domain/control/configuration/parsers/logging_control_configuration_cbor_parser.cpp
  ${RT_CORE_DIR}/src/domain/logging_domain/control/configuration/parsers/logging_control_configuration_validator.cpp
  ${RT_CORE_DIR}/src/configuration/cbor/cbor_logging_control_config/cbor_logging_control_config_cbor_encode.cpp
  ${RT_CORE_DIR}/src/configuration/cbor/cbor_logging_control_config/cbor_logging_control_config_cbor_decode.cpp
  ${RT_CORE_DIR}/src/configuration/cbor/traits/logging_control_config_trait.cpp
)

# MemoryStreamBuf: std::stringbuf seeks do not link on every supported platform.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../common)
