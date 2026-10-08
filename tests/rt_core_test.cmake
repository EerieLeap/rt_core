# Shared setup for the eerie_leap_rt_core twister suites.
#
# Must be included before find_package(Zephyr) so that EXTRA_ZEPHYR_MODULES is
# honoured. The Lua wrapper and the expression engine are submodules of rt_core
# (modules/), so they are available in every checkout; Zephyr does not discover
# nested modules, so they are registered here.

get_filename_component(RT_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(EXTRA_ZEPHYR_MODULES
    "${RT_CORE_DIR}/modules/zephyr_lua"
    "${RT_CORE_DIR}/modules/expression_engine"
    "${RT_CORE_DIR}")

if(BOARD MATCHES "^qemu_cortex_a")
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/qemu_mmu.conf")
endif()

# qemu_malta only declares 1 MB of SRAM; the machine actually provides far more.
if(BOARD MATCHES "^qemu_malta")
    list(APPEND EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_LIST_DIR}/qemu_malta_ram.overlay")
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/qemu_malta.conf")
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
