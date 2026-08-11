# Shared setup for the eerie_leap_rt_core twister suites.
#
# Must be included before find_package(Zephyr) so that EXTRA_ZEPHYR_MODULES is
# honoured. When rt_core is the west manifest repository both modules are
# discovered automatically and the explicit registration below is a no-op.

get_filename_component(RT_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(RT_CORE_PARENT_DIR "${RT_CORE_DIR}/.." ABSOLUTE)

set(EXTRA_ZEPHYR_MODULES "${RT_CORE_DIR}")

foreach(candidate
    "${RT_CORE_PARENT_DIR}/eerie_leap_lua"
    "${RT_CORE_PARENT_DIR}/modules/eerie_leap_lua")
    if(EXISTS "${candidate}/zephyr/module.yml")
        list(APPEND EXTRA_ZEPHYR_MODULES "${candidate}")
        break()
    endif()
endforeach()

# Boost is linked through the rt_core module but has to be configured by the
# application, so the suites must mirror what app/CMakeLists.txt sets.
add_definitions(-DBOOST_SYSTEM_DISABLE_THREADS)
add_definitions(-DBOOST_JSON_STACK_BUFFER_SIZE=1024)
add_definitions(-DBOOST_HAS_STDINT_H)

if(BOARD MATCHES "^qemu_cortex_a")
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/qemu_mmu.conf")
endif()

# qemu_malta only declares 1 MB of SRAM; the machine actually provides far more.
if(BOARD MATCHES "^qemu_malta")
    list(APPEND EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_LIST_DIR}/qemu_malta_ram.overlay")
endif()

# Suites that mount the internal filesystem set RT_CORE_TEST_SIM_FLASH before
# including this file. native_sim already ships a simulated flash controller.
if(RT_CORE_TEST_SIM_FLASH AND BOARD MATCHES "^qemu_")
    list(APPEND EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_LIST_DIR}/qemu_flash.overlay")
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/qemu_flash.conf")
endif()

# Suites that mount a FAT volume on top of a flash disk.
if(RT_CORE_TEST_FLASH_DISK AND BOARD MATCHES "^qemu_")
    list(APPEND EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_LIST_DIR}/qemu_flash_disk.overlay")
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/qemu_flash.conf")
endif()

# Likewise for suites that need emulated ADCs.
if(RT_CORE_TEST_ADC_EMUL AND BOARD MATCHES "^qemu_")
    list(APPEND EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_LIST_DIR}/qemu_adc.overlay")
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/qemu_adc.conf")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
