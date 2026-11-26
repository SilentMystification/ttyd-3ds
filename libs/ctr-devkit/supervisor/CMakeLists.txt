# SPDX-License-Identifier: MIT
# Copyright (c) the ctr-devkit authors
#
# The supervisor system module (development only) and its app build for emulators.
# Builds standalone:
#     cmake -S libs/ctr-devkit/supervisor -B build/ctrdk-supervisor -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake && cmake --build build/ctrdk-supervisor
# (in this repository: tools/port3ds/build.sh libs/ctr-devkit/supervisor).
# Gives ctrdk-supervisor.cxi and ctrdk-supervisor.cia (with makerom) and
# ctrdk-supervisor-app.3dsx. The in-app client library, ctrdk_supervisor_client,
# is defined by libs/ctr-devkit/CMakeLists.txt.

cmake_minimum_required(VERSION 3.20)
project(ctrdk_supervisor C)

if(NOT TARGET ctrdk_supervisor_client)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/.." ctr-devkit)
endif()

set(sv_common
    "${CMAKE_CURRENT_LIST_DIR}/server.c"
    "${CMAKE_CURRENT_LIST_DIR}/actions.c"
    "${CMAKE_CURRENT_LIST_DIR}/hb.c"
    "${CMAKE_CURRENT_LIST_DIR}/policy.c"
    "${CMAKE_CURRENT_LIST_DIR}/../devupdate/proto.c")

# The system module.
add_executable(ctrdk_supervisor "${CMAKE_CURRENT_LIST_DIR}/main.c" ${sv_common})
target_compile_options(ctrdk_supervisor PRIVATE -O2 -g -Wall -Wextra -Werror)
target_include_directories(ctrdk_supervisor PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../include")
target_link_libraries(ctrdk_supervisor PRIVATE ctru m)
target_link_options(ctrdk_supervisor PRIVATE -Wl,--wrap=svcBreak)
ctrdk_build_stamp(ctrdk_supervisor)

if(CTRDK_MAKEROM)
    set(out "${CMAKE_CURRENT_BINARY_DIR}/ctrdk-supervisor")
    set(rsf "${CMAKE_CURRENT_LIST_DIR}/supervisor.rsf")
    add_custom_command(
        OUTPUT "${out}.cxi"
        COMMAND "${CTRDK_MAKEROM}" -f ncch -rsf "${rsf}" -elf "$<TARGET_FILE:ctrdk_supervisor>" -o "${out}.cxi"
        DEPENDS ctrdk_supervisor "${rsf}"
        VERBATIM
        COMMENT "Building ctrdk-supervisor.cxi")
    add_custom_command(
        OUTPUT "${out}.cia"
        COMMAND "${CTRDK_MAKEROM}" -f cia -i "${out}.cxi:0:0" -o "${out}.cia"
        DEPENDS "${out}.cxi"
        VERBATIM
        COMMENT "Building ctrdk-supervisor.cia")
    add_custom_target(ctrdk_supervisor_cia ALL DEPENDS "${out}.cia")
else()
    message(STATUS "ctrdk_supervisor: makerom not found, building the ELF only")
endif()

# The same server as an ordinary app, for Azahar (it cannot start a custom system module).
add_executable(ctrdk_supervisor_app "${CMAKE_CURRENT_LIST_DIR}/app_main.c" ${sv_common})
target_compile_options(ctrdk_supervisor_app PRIVATE -O2 -g -Wall -Wextra -Werror)
target_include_directories(ctrdk_supervisor_app PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../include")
# The app build sends itself heartbeats through the real client.
target_link_libraries(ctrdk_supervisor_app PRIVATE ctrdk_supervisor_client ctru m)
ctrdk_build_stamp(ctrdk_supervisor_app)
set(app_out "${CMAKE_CURRENT_BINARY_DIR}/ctrdk-supervisor-app")
ctr_generate_smdh(OUTPUT "${app_out}.smdh" NAME "ctr-devkit supervisor (app)"
    DESCRIPTION "supervisor protocol test build" AUTHOR "ctr-devkit" ICON "${CTRDK_CMAKE_DIR}/art/icon.png")
ctr_create_3dsx(ctrdk_supervisor_app OUTPUT "${app_out}.3dsx" SMDH "${app_out}.smdh")
