# SPDX-License-Identifier: MIT
# Copyright (c) the ctr-devkit authors
#
# ctr-devkit libraries. Use from another project:
#     add_subdirectory(path/to/ctr-devkit ctr-devkit)
#     target_link_libraries(app PRIVATE ctrdk_crash ctrdk_devupdate)
#     ctrdk_wrap_svcbreak(app)          # svcBreak becomes a crash report
# Built on its own (cmake -S libs/ctr-devkit with devkitPro's 3DS toolchain)
# it also builds the example apps.

cmake_minimum_required(VERSION 3.20)
project(ctr_devkit C)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/CtrDevkitApp.cmake")

set(CTRDK_FLAGS -O2 -g -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections)

add_library(ctrdk_crash STATIC "${CMAKE_CURRENT_LIST_DIR}/crash/crash.c")
target_include_directories(ctrdk_crash PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_options(ctrdk_crash PRIVATE ${CTRDK_FLAGS})

add_library(ctrdk_devupdate STATIC
    "${CMAKE_CURRENT_LIST_DIR}/devupdate/devupdate.c"
    "${CMAKE_CURRENT_LIST_DIR}/devupdate/files.c"
    "${CMAKE_CURRENT_LIST_DIR}/devupdate/queue.c")
target_include_directories(ctrdk_devupdate PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_options(ctrdk_devupdate PRIVATE ${CTRDK_FLAGS})

# svcBreak (libctru and citro3d panics) goes to the crash handler.
function(ctrdk_wrap_svcbreak target)
    target_link_options(${target} PRIVATE -Wl,--wrap=svcBreak)
endfunction()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_subdirectory(examples/crash)
    add_subdirectory(examples/devupdate)
endif()
