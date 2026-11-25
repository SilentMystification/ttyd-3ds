# SPDX-License-Identifier: MIT
# Copyright (c) the ctr-devkit authors
#
# ctrdk_gpuprobe: PICA200 rules probe (include/ctrdk/gpuprobe.h). libctru and citro3d only;
# its shaders are embedded, so the app adds nothing but the link:
#     target_link_libraries(app PRIVATE ctrdk_gpuprobe)

enable_language(ASM)

ctr_add_shader_library(gpuprobe_shader OUTPUT gpuprobe.shbin
    "${CMAKE_CURRENT_LIST_DIR}/shaders/normal.v.pica"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/partial_pos.v.pica"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/partial_clr.v.pica"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/partial_tc.v.pica"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/bool15.v.pica"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/bool14.v.pica")
dkp_add_embedded_binary_library(gpuprobe_shader_bin gpuprobe_shader)

add_library(ctrdk_gpuprobe STATIC "${CMAKE_CURRENT_LIST_DIR}/gpuprobe.c")
target_include_directories(ctrdk_gpuprobe PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
target_compile_options(ctrdk_gpuprobe PRIVATE ${CTRDK_FLAGS})
target_link_libraries(ctrdk_gpuprobe PRIVATE gpuprobe_shader_bin)
target_link_libraries(ctrdk_gpuprobe PUBLIC citro3d ctru)
