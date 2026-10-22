# The identity a signed image carries, and a unit checks while the image uploads.
#
# The identity is the value of a protected (signed) MCUboot TLV, tag EERIE_LEAP_IMAGE_IDENTITY_TLV_TAG:
#
#   [format u8 = 1][product_id u16 little-endian][board_len u8][board_target, board_len ASCII bytes]
#
# It is the CMake side of the unit's ImageIdentity (src/subsys/dfu/). Include this file to sign
# an image the way a build does, for example in a test:
#
#   include(<rt_core>/cmake/image_identity.cmake)
#   eerie_leap_image_identity_hex(4660 "native_sim/native" hex)
#   execute_process(COMMAND imgtool sign ... --custom-tlv ${EERIE_LEAP_IMAGE_IDENTITY_TLV_TAG} ${hex} ...)
#
# or from a shell:
#
#   cmake -DPRODUCT_ID=4660 -DBOARD_TARGET=native_sim/native -P image_identity.cmake

# Per directory, not global: the variables below belong to the directory scope that includes the file, and
# the signing wrapper includes it in Zephyr's own scope before an application's CMakeLists.txt does.
include_guard(DIRECTORY)

set(EERIE_LEAP_IMAGE_IDENTITY_TLV_TAG 0xa0)
set(EERIE_LEAP_IMAGE_IDENTITY_FORMAT 1)

# The unit accepts at most 128 B of protected TLVs (the TLV info header, 4 B, and each TLV's header, 4 B,
# count), so the identity, alone in the area, is 4 B + the board target at most 116 B long.
set(EERIE_LEAP_IMAGE_IDENTITY_BOARD_MAX_LEN 116)

# One byte as two lowercase hex digits.
function(_eerie_leap_hex_byte value out_var)
    # Adding 256 gives three hex digits whatever the value, "0x1" and then the byte.
    math(EXPR with_marker "${value} + 256" OUTPUT_FORMAT HEXADECIMAL)
    string(SUBSTRING "${with_marker}" 3 2 byte)
    set(${out_var} "${byte}" PARENT_SCOPE)
endfunction()

# eerie_leap_image_identity_hex(<product_id> <board_target> <out_var>)
#
# Sets <out_var> to the identity as "0x" and lowercase hex digits, the value imgtool's
# --custom-tlv takes. <product_id> is 0..65535 and <board_target> the CONFIG_BOARD_TARGET string.
function(eerie_leap_image_identity_hex product_id board_target out_var)
    if(NOT "${product_id}" MATCHES "^[0-9]+$" OR "${product_id}" GREATER 65535)
        message(FATAL_ERROR "Image identity: the product ID must be 0..65535, got '${product_id}'.")
    endif()

    string(LENGTH "${board_target}" board_len)
    if(board_len EQUAL 0 OR board_len GREATER ${EERIE_LEAP_IMAGE_IDENTITY_BOARD_MAX_LEN})
        message(FATAL_ERROR "Image identity: the board target must be 1..${EERIE_LEAP_IMAGE_IDENTITY_BOARD_MAX_LEN} "
                            "characters, got ${board_len} ('${board_target}').")
    endif()

    # A multi-byte character would make the length in characters differ from the one in bytes.
    if(NOT "${board_target}" MATCHES "^[ -~]+$")
        message(FATAL_ERROR "Image identity: the board target must be printable ASCII, got '${board_target}'.")
    endif()

    math(EXPR product_low "${product_id} & 255")
    math(EXPR product_high "(${product_id} >> 8) & 255")

    _eerie_leap_hex_byte(${EERIE_LEAP_IMAGE_IDENTITY_FORMAT} format_hex)
    _eerie_leap_hex_byte(${product_low} product_low_hex)
    _eerie_leap_hex_byte(${product_high} product_high_hex)
    _eerie_leap_hex_byte(${board_len} board_len_hex)
    string(HEX "${board_target}" board_hex)

    set(${out_var} "0x${format_hex}${product_low_hex}${product_high_hex}${board_len_hex}${board_hex}" PARENT_SCOPE)
endfunction()

# Run as a script (cmake -P): print the identity of PRODUCT_ID and BOARD_TARGET.
if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    if(NOT DEFINED PRODUCT_ID OR NOT DEFINED BOARD_TARGET)
        message(FATAL_ERROR "Usage: cmake -DPRODUCT_ID=<0..65535> -DBOARD_TARGET=<board target> -P image_identity.cmake")
    endif()

    eerie_leap_image_identity_hex("${PRODUCT_ID}" "${BOARD_TARGET}" identity_hex)
    execute_process(COMMAND ${CMAKE_COMMAND} -E echo "${identity_hex}")
endif()
