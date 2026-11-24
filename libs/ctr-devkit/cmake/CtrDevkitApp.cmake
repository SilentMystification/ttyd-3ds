# SPDX-License-Identifier: MIT
# Copyright (c) the ctr-devkit authors
#
# ctrdk_add_app(<target>
#     TITLE <text> UNIQUE_ID <0x...> PRODUCT_CODE <CTR-N-XXXX>
#     [DESCRIPTION <text>] [AUTHOR <text>] [OUTPUT_NAME <name>] [RSF <file>])
#
# Builds <name>.3dsx (devkitPro's 3dsxtool and smdhtool) and, when makerom
# and bannertool are on PATH, <name>.cia next to the ELF of executable
# <target>. The art is the placeholder set in cmake/art/; the CIA uses
# cmake/app.rsf unless RSF names another (e.g. one with New 3DS settings). UNIQUE_ID must be a homebrew-range ID (0xF8000-0xFFFFF)
# that no other installed title uses.

# A cache variable, so the functions also find it from a project that pulled
# ctr-devkit in with add_subdirectory (the standalone example builds).
set(CTRDK_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "ctr-devkit cmake/ folder")
find_program(CTRDK_MAKEROM makerom)
find_program(CTRDK_BANNERTOOL bannertool)

function(ctrdk_add_app target)
    cmake_parse_arguments(ARG "" "TITLE;UNIQUE_ID;PRODUCT_CODE;DESCRIPTION;AUTHOR;OUTPUT_NAME;RSF" "" ${ARGN})
    foreach(req TITLE UNIQUE_ID PRODUCT_CODE)
        if(NOT ARG_${req})
            message(FATAL_ERROR "ctrdk_add_app(${target}): ${req} is required")
        endif()
    endforeach()
    if(NOT ARG_DESCRIPTION)
        set(ARG_DESCRIPTION "${ARG_TITLE}")
    endif()
    if(NOT ARG_AUTHOR)
        set(ARG_AUTHOR "ctr-devkit")
    endif()
    if(NOT ARG_OUTPUT_NAME)
        set(ARG_OUTPUT_NAME "${target}")
    endif()
    if(NOT ARG_RSF)
        set(ARG_RSF "${CTRDK_CMAKE_DIR}/app.rsf")
    endif()
    set(art "${CTRDK_CMAKE_DIR}/art")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/${ARG_OUTPUT_NAME}")

    ctr_generate_smdh(OUTPUT "${out}.smdh" NAME "${ARG_TITLE}" DESCRIPTION "${ARG_DESCRIPTION}"
        AUTHOR "${ARG_AUTHOR}" ICON "${art}/icon.png")
    ctr_create_3dsx(${target} OUTPUT "${out}.3dsx" SMDH "${out}.smdh")

    if(NOT CTRDK_MAKEROM OR NOT CTRDK_BANNERTOOL)
        message(STATUS "ctrdk_add_app(${target}): makerom or bannertool not found, building the 3dsx only")
        return()
    endif()
    add_custom_command(
        OUTPUT "${out}.bnr"
        COMMAND "${CTRDK_BANNERTOOL}" makebanner -i "${art}/banner.png" -a "${art}/banner.wav" -o "${out}.bnr"
        DEPENDS "${art}/banner.png" "${art}/banner.wav"
        VERBATIM)
    add_custom_command(
        OUTPUT "${out}.cia"
        COMMAND "${CTRDK_MAKEROM}" -f cia -o "${out}.cia" -target t -exefslogo
                -elf "$<TARGET_FILE:${target}>" -rsf "${ARG_RSF}"
                -icon "${out}.smdh" -banner "${out}.bnr"
                "-DAPP_TITLE=${ARG_TITLE}" "-DAPP_PRODUCT_CODE=${ARG_PRODUCT_CODE}"
                "-DAPP_UNIQUE_ID=${ARG_UNIQUE_ID}"
        DEPENDS ${target} "${ARG_RSF}" "${out}.smdh" "${out}.bnr"
        VERBATIM
        COMMENT "Building ${ARG_OUTPUT_NAME}.cia")
    add_custom_target(${target}_cia ALL DEPENDS "${out}.cia")
endfunction()

# A header with a build stamp that changes on every build:
#   #define CTRDK_BUILD_STAMP "2026-11-22 14:03:07"
# so a pushed build is visibly new. ctrdk_build_stamp(<target>) adds it to
# the target's include path.
function(ctrdk_build_stamp target)
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_stamp")
    file(MAKE_DIRECTORY "${dir}")
    add_custom_target(${target}_stamp
        COMMAND "${CMAKE_COMMAND}" "-DOUT=${dir}/build_stamp.h" -P "${CTRDK_CMAKE_DIR}/BuildStamp.cmake"
        BYPRODUCTS "${dir}/build_stamp.h"
        VERBATIM)
    add_dependencies(${target} ${target}_stamp)
    target_include_directories(${target} PRIVATE "${dir}")
endfunction()
