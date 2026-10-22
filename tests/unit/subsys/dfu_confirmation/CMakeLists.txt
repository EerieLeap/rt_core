cmake_minimum_required(VERSION 3.20.0)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../rt_core_test.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(unit_subsys_dfu_confirmation)

file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.cpp)

# The sources under test are compiled straight into the test. CONFIG_EERIE_LEAP_DFU would also pull in
# img_mgmt, the flash simulator and the SMP stack, none of which these cases use. The two Zephyr
# implementations are included to prove that they compile (as stubs) without MCUboot and a watchdog.
set(DFU_SRC_DIR ${RT_CORE_DIR}/src/subsys/dfu)

target_sources(app PRIVATE
    ${TEST_SOURCES}
    ${DFU_SRC_DIR}/image_confirmation.cpp
    ${DFU_SRC_DIR}/mcuboot_boot_control.cpp
    ${DFU_SRC_DIR}/work_queue_probe.cpp
    ${DFU_SRC_DIR}/zephyr_watchdog.cpp
)
target_include_directories(app PRIVATE ${RT_CORE_DIR}/src)
