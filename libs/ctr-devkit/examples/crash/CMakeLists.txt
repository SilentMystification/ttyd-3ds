# SPDX-License-Identifier: MIT
# Copyright (c) the ctr-devkit authors
#
# The crash example. Builds standalone:
#     cmake -S libs/ctr-devkit/examples/crash -B build/crash-example -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake && cmake --build build/crash-example
# Gives ctrdk-crash.3dsx (and ctrdk-crash.cia with makerom and bannertool).

cmake_minimum_required(VERSION 3.20)
project(ctrdk_example_crash C)

if(NOT TARGET ctrdk_crash)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../.." ctr-devkit)
endif()

add_executable(ctrdk_example_crash main.c)
target_compile_options(ctrdk_example_crash PRIVATE -O2 -g -Wall -Wextra -Werror)
target_link_libraries(ctrdk_example_crash PRIVATE ctrdk_crash ctru m)
ctrdk_wrap_svcbreak(ctrdk_example_crash)

# Title 000400000FD88800 (homebrew range; the TTYD port uses 0xFD886 and its probe 0xFD887).
ctrdk_add_app(ctrdk_example_crash
    OUTPUT_NAME ctrdk-crash
    TITLE "ctr-devkit crash"
    DESCRIPTION "ctr-devkit crash handler example"
    UNIQUE_ID 0xFD888
    PRODUCT_CODE CTR-N-CDKC)
