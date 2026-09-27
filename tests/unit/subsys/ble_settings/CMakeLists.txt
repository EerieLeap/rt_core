cmake_minimum_required(VERSION 3.20.0)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../rt_core_test.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(unit_subsys_ble_settings)

# The settings commands don't touch the Bluetooth stack, so they're built here without CONFIG_BT.
file(GLOB BLE_SETTINGS_COMMAND_SOURCES
    ${RT_CORE_DIR}/src/subsys/bluetooth/ble_settings/ble_settings_command/*.cpp)
file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.cpp)

target_sources(app PRIVATE ${TEST_SOURCES} ${BLE_SETTINGS_COMMAND_SOURCES})
