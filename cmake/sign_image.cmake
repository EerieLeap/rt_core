# Signing wrapper for the MCUboot images of rt_core units.
#
# rt_core's CMakeLists.txt sets it as Zephyr's SIGNING_SCRIPT (read in zephyr/CMakeLists.txt after the
# modules) when CONFIG_BOOTLOADER_MCUBOOT is set. It adds to CONFIG_MCUBOOT_EXTRA_IMGTOOL_ARGS, which
# Zephyr's cmake/mcuboot.cmake passes to every `imgtool sign`, and then includes mcuboot.cmake:
#
#   --max-sectors <sectors in slot0>
#       The swap status trailer at the end of a slot grows with the sector count. imgtool assumes 128
#       sectors unless told otherwise, far fewer than a gauge slot has (hundreds of 4 KB sectors), so
#       it would accept an image that does not fit once the real trailer is reserved. With the real
#       count, its overflow check is right.
#
#   --custom-tlv 0xa0 0x<identity>
#       The image identity (product and board) in a protected TLV, so it is covered by the signature.
#       Only when CONFIG_EERIE_LEAP_PRODUCT_ID exists, which a unit with the system domain has. See
#       image_identity.cmake for the layout.
#
# A build that sets its own SIGNING_SCRIPT (sysbuild's SIGNING_SCRIPT variable takes precedence over the
# property) bypasses this file.

include(${CMAKE_CURRENT_LIST_DIR}/image_identity.cmake)

# Appends the arguments to CONFIG_MCUBOOT_EXTRA_IMGTOOL_ARGS in the caller's scope.
function(eerie_leap_sign_image_extra_args)
    set(args "${CONFIG_MCUBOOT_EXTRA_IMGTOOL_ARGS}")

    # --max-sectors: the sectors of slot0, unless the build passes its own.
    if("${args}" MATCHES "(^| )(--max-sectors|-M)( |=|$)")
        message(STATUS "Image signing: --max-sectors comes from CONFIG_MCUBOOT_EXTRA_IMGTOOL_ARGS")
    else()
        dt_nodelabel(slot0_node NODELABEL "slot0_partition")
        dt_chosen(flash_node PROPERTY "zephyr,flash")

        if(slot0_node AND flash_node)
            dt_prop(slot_size PATH "${slot0_node}" PROPERTY "reg" INDEX 1)
            dt_prop(erase_block_size PATH "${flash_node}" PROPERTY "erase-block-size")
        endif()

        if(slot_size AND erase_block_size)
            math(EXPR max_sectors "${slot_size} / ${erase_block_size}")
            string(APPEND args " --max-sectors ${max_sectors}")
            message(STATUS "Image signing: --max-sectors ${max_sectors} (slot0 ${slot_size} B, erase block ${erase_block_size} B)")
        else()
            # A flash with a layout (erase-blocks) has no one sector size.
            message(WARNING "Image signing: slot0_partition or the flash's erase-block-size is missing from the "
                            "devicetree, imgtool keeps its default of 128 sectors.")
        endif()
    endif()

    # The identity TLV.
    if(DEFINED CONFIG_EERIE_LEAP_PRODUCT_ID)
        eerie_leap_image_identity_hex("${CONFIG_EERIE_LEAP_PRODUCT_ID}" "${CONFIG_BOARD_TARGET}" identity_hex)
        string(APPEND args " --custom-tlv ${EERIE_LEAP_IMAGE_IDENTITY_TLV_TAG} ${identity_hex}")
        message(STATUS "Image signing: identity TLV ${EERIE_LEAP_IMAGE_IDENTITY_TLV_TAG} ${identity_hex} "
                       "(product ${CONFIG_EERIE_LEAP_PRODUCT_ID}, board ${CONFIG_BOARD_TARGET})")
    endif()

    string(STRIP "${args}" args)
    set(CONFIG_MCUBOOT_EXTRA_IMGTOOL_ARGS "${args}" PARENT_SCOPE)
endfunction()

eerie_leap_sign_image_extra_args()

include(${ZEPHYR_BASE}/cmake/mcuboot.cmake)
