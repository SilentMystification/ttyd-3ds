# SPDX-License-Identifier: MIT
# Copyright (c) the ctr-devkit authors
#
# ctr-devkit libraries. Use from another project:
#     add_subdirectory(path/to/ctr-devkit ctr-devkit)
#     target_link_libraries(app PRIVATE ctrdk_crash ctrdk_devupdate ctrdk_perf)
#     ctrdk_wrap_svcbreak(app)          # svcBreak becomes a crash report
# Built on its own (cmake -S libs/ctr-devkit with devkitPro's 3DS toolchain)
# it also builds the example apps.

cmake_minimum_required(VERSION 3.20)
project(ctr_devkit C ASM)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/CtrDevkitApp.cmake")

set(CTRDK_FLAGS -O2 -g -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections)

add_library(ctrdk_crash STATIC "${CMAKE_CURRENT_LIST_DIR}/crash/crash.c")
target_include_directories(ctrdk_crash PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_options(ctrdk_crash PRIVATE ${CTRDK_FLAGS})

add_library(ctrdk_devupdate STATIC
    "${CMAKE_CURRENT_LIST_DIR}/devupdate/devupdate.c"
    "${CMAKE_CURRENT_LIST_DIR}/devupdate/files.c"
    "${CMAKE_CURRENT_LIST_DIR}/devupdate/proto.c"
    "${CMAKE_CURRENT_LIST_DIR}/devupdate/queue.c")
target_include_directories(ctrdk_devupdate PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_options(ctrdk_devupdate PRIVATE ${CTRDK_FLAGS})

# ARM11 PMU and SCU counters (svcControlPerformanceCounter), for per-frame logging.
add_library(ctrdk_perf STATIC "${CMAKE_CURRENT_LIST_DIR}/perf/perf.c")
target_include_directories(ctrdk_perf PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_options(ctrdk_perf PRIVATE ${CTRDK_FLAGS})

# Supervisor client (development only, optional): install, start, check and remove the
# supervisor system module from the app. The module itself: supervisor/CMakeLists.txt.
add_library(ctrdk_supervisor_client STATIC "${CMAKE_CURRENT_LIST_DIR}/supervisor/client.c")
target_include_directories(ctrdk_supervisor_client PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_options(ctrdk_supervisor_client PRIVATE ${CTRDK_FLAGS})

# PICA200 rules probe (own shaders, citro3d).
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/gpuprobe" gpuprobe)

# svcBreak (libctru and citro3d panics) goes to the crash handler.
function(ctrdk_wrap_svcbreak target)
    target_link_options(${target} PRIVATE -Wl,--wrap=svcBreak)
endfunction()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_subdirectory(examples/crash)
    add_subdirectory(examples/devupdate)
    add_subdirectory(qtmbench)
    add_subdirectory(supervisor)
endif()
