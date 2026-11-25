# SPDX-License-Identifier: MIT
# Copyright (c) the ctr-devkit authors
#
# qtmbench: New 3DS memory benchmark (QTM extra RAM vs FCRAM heap/linear vs
# VRAM; CPU and GPU), with devupdate built in. Builds with the rest of
# ctr-devkit, or standalone:
#     cmake -S libs/ctr-devkit/qtmbench -B build/qtmbench -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake && cmake --build build/qtmbench
# (in this repository: tools/port3ds/build.sh libs/ctr-devkit/qtmbench)
# Gives qtmbench.3dsx and qtmbench.cia (title 000400000FD88C00).

cmake_minimum_required(VERSION 3.20)
project(ctrdk_qtmbench C ASM)

if(NOT TARGET ctrdk_devupdate)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/.." ctr-devkit)
endif()

ctr_add_shader_library(qtmbench_shader OUTPUT qtmbench.shbin "${CMAKE_CURRENT_LIST_DIR}/source/bench.v.pica")
dkp_add_embedded_binary_library(qtmbench_shader_bin qtmbench_shader)

add_executable(qtmbench
    source/main.c
    source/out.c
    source/mem.c
    source/cpu.c
    source/gpu.c
    source/contend.c
    source/kern.s)
target_compile_options(qtmbench PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -g -Wall -Wextra -Werror -Wno-unused-parameter>)
target_link_libraries(qtmbench PRIVATE qtmbench_shader_bin ctrdk_devupdate ctrdk_crash ctrdk_perf ctrdk_gpuprobe citro3d ctru m)
ctrdk_wrap_svcbreak(qtmbench)
ctrdk_build_stamp(qtmbench)

# 0xFD886 is the TTYD port (0xFD88A/0xFD88B its JP/PAL builds), 0xFD887 its
# probe, 0xFD888/0xFD889 the crash and devupdate examples.
ctrdk_add_app(qtmbench
    TITLE "qtmbench"
    DESCRIPTION "New 3DS memory benchmark (QTM, FCRAM, VRAM)"
    UNIQUE_ID 0xFD88C
    PRODUCT_CODE CTR-N-CDKQ
    RSF "${CMAKE_CURRENT_LIST_DIR}/qtmbench.rsf")
