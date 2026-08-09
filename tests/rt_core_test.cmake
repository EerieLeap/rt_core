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

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
