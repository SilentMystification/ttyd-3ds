# SPDX-License-Identifier: MIT
# Copyright (c) the ctr-devkit authors
#
# The devupdate example, and the generic bootstrap. Builds standalone:
#     cmake -S libs/ctr-devkit/examples/devupdate -B build/devupdate-example -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake && cmake --build build/devupdate-example
# Gives devupdate-boot.3dsx (the bootstrap) and ctrdk-devupdate.cia (with
# makerom and bannertool).

cmake_minimum_required(VERSION 3.20)
project(ctrdk_example_devupdate C)

if(NOT TARGET ctrdk_devupdate)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../.." ctr-devkit)
endif()

add_executable(ctrdk_example_devupdate main.c)
target_compile_options(ctrdk_example_devupdate PRIVATE -O2 -g -Wall -Wextra -Werror)
target_link_libraries(ctrdk_example_devupdate PRIVATE ctrdk_devupdate ctrdk_crash ctru m)
ctrdk_wrap_svcbreak(ctrdk_example_devupdate)
ctrdk_build_stamp(ctrdk_example_devupdate)

# Title 000400000FD88900 (homebrew range; the TTYD port uses 0xFD886, its probe 0xFD887,
# the crash example 0xFD888).
ctrdk_add_app(ctrdk_example_devupdate
    OUTPUT_NAME ctrdk-devupdate
    TITLE "ctr-devkit devupdate"
    DESCRIPTION "ctr-devkit devupdate example and bootstrap"
    UNIQUE_ID 0xFD889
    PRODUCT_CODE CTR-N-CDKU)
# The same executable as the bootstrap's name.
add_custom_command(TARGET ctrdk_example_devupdate_3dsx POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_BINARY_DIR}/ctrdk-devupdate.3dsx"
            "${CMAKE_CURRENT_BINARY_DIR}/devupdate-boot.3dsx"
    VERBATIM)
