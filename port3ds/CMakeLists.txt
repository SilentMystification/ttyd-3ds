# TTYD 3DS port: the game build.
#
#   tools/port3ds/build.sh            (Docker, Linux)
#   tools\port3ds\build.ps1           (Docker, Windows)
#
# Compiles the decomp's game code (src/, rels/) and MusyX for the ARM11 with
# the port SDK in port3ds/. See docs/PORT_3DS_PLAN.md.

cmake_minimum_required(VERSION 3.20)
project(ttyd3ds C ASM)

set(TTYD_VERSION G8ME01 CACHE STRING "Game version (G8ME01; G8MJ01 and G8MP01 in phase 7)")
set_property(CACHE TTYD_VERSION PROPERTY STRINGS G8ME01)

get_filename_component(TTYD_REPO_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/sources_${TTYD_VERSION}.cmake")

# BUILD_VERSION is the index in configure.py's VERSIONS list.
set(TTYD_BUILD_VERSION_G8MJ01 0)
set(TTYD_BUILD_VERSION_G8ME01 1)
set(TTYD_BUILD_VERSION_G8MP01 2)

# Flags for all game and port C:
#   -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks: the decomp
#     was written for MWCC, which does none of these optimisations.
#   plain char stays unsigned (ARM default, same as MWCC -char unsigned).
#   -fno-short-enums: enums are int-sized, as MWCC -enum int (devkitARM's
#     default is short enums). Game-side code only: see the ABI note below.
#   -ffp-contract=off: VFPv2 has no fused multiply-add; keep results stable.
#   -fgnu89-inline: a plain 'inline' function also gets an external
#     definition, as with MWCC. -fcommon: tentative definitions merge, as
#     with MWCC.
#   -iquote include/sdk: bare "texPalette.h" & co. resolve there, as MWCC's
#     access paths do (same as paper-cut).
#   Implicit function declarations stay errors (a call without a prototype
#   truncates pointers and passes floats as doubles). int-conversion and
#   incompatible-pointer-types are warnings, as MWCC accepts them;
#   TTYD_WARNINGS=ON shows them for auditing.
option(TTYD_WARNINGS "Show compiler warnings in game code" OFF)
set(TTYD_C_FLAGS
    -std=gnu99 -O2 -g -fno-short-enums
    -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks
    -ffp-contract=off -fgnu89-inline -fcommon
    -ffunction-sections -fdata-sections
    -include "${CMAKE_CURRENT_LIST_DIR}/include/port3ds_prefix.h"
    "SHELL:-iquote ${TTYD_REPO_DIR}/include/sdk"
    -Werror=implicit-function-declaration
    -Wno-error=int-conversion -Wno-error=incompatible-pointer-types
)
if(NOT TTYD_WARNINGS)
    list(APPEND TTYD_C_FLAGS -w)
endif()
set(TTYD_DEFINES
    VERSION_${TTYD_VERSION}
    BUILD_VERSION=${TTYD_BUILD_VERSION_${TTYD_VERSION}}
    SDK_REVISION=1
    NDEBUG
    # MusyX 2.0.3 (configure.py), built for its PC target: the 3DS backend
    # (port3ds/audio) replaces the DSP and ARAM hardware layer.
    MUSY_VERSION_MAJOR=2 MUSY_VERSION_MINOR=0 MUSY_VERSION_PATCH=3
    MUSY_TARGET=0
)
set(TTYD_INCLUDES
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${TTYD_REPO_DIR}/include"
    "${TTYD_REPO_DIR}/libs/dolsdk2004/include"
    "${TTYD_REPO_DIR}/libs/musyx/include"
)

function(ttyd_game_objects name)
    add_library(${name} OBJECT ${ARGN})
    target_compile_options(${name} PRIVATE ${TTYD_C_FLAGS})
    target_compile_definitions(${name} PRIVATE ${TTYD_DEFINES})
    target_include_directories(${name} PRIVATE ${TTYD_INCLUDES})
endfunction()

ttyd_game_objects(ttyd_dol ${TTYD_DOL_SOURCES})
# The decomp's main() runs inside the port's main() (port3ds/plat/boot.c).
set_source_files_properties("${TTYD_REPO_DIR}/src/main.c" TARGET_DIRECTORY ttyd_dol
    PROPERTIES COMPILE_DEFINITIONS "main=ttyd_main")
ttyd_game_objects(ttyd_musyx ${TTYD_MUSYX_SOURCES})

# Static RELs: each REL's objects are partially linked into one object with
# local symbols (tools/port3ds/rel_link.py), and a generated table lists
# their entry points and data/bss bounds for the port's OSLink.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/TtydCia.cmake")

# Build number: commits on the branch and the short hash, shown in the log,
# the HUD and crash reports.
find_package(Git QUIET)
set(TTYD_BUILD_COUNT 0)
set(TTYD_BUILD_HASH unknown)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-list --count HEAD WORKING_DIRECTORY "${TTYD_REPO_DIR}"
        OUTPUT_VARIABLE TTYD_BUILD_COUNT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD WORKING_DIRECTORY "${TTYD_REPO_DIR}"
        OUTPUT_VARIABLE TTYD_BUILD_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
set(TTYD_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(TTYD_REL_OBJECTS)
set(_rel_decls "")
set(_rel_rows "")
foreach(rel ${TTYD_RELS})
    ttyd_game_objects(ttyd_rel_${rel} ${TTYD_REL_${rel}_SOURCES})
    if(EXISTS "${TTYD_REPO_DIR}/rels/${rel}/include")
        target_include_directories(ttyd_rel_${rel} PRIVATE "${TTYD_REPO_DIR}/rels/${rel}/include")
    endif()
    set(_rel_obj "${CMAKE_CURRENT_BINARY_DIR}/rel/rel_${rel}.o")
    add_custom_command(
        OUTPUT "${_rel_obj}"
        COMMAND "${Python3_EXECUTABLE}" -I "${TTYD_REPO_DIR}/tools/port3ds/rel_link.py"
                --rel ${rel} --ld "${CMAKE_LINKER}" --objcopy "${CMAKE_OBJCOPY}"
                --out "${_rel_obj}" $<TARGET_OBJECTS:ttyd_rel_${rel}>
        DEPENDS ttyd_rel_${rel} $<TARGET_OBJECTS:ttyd_rel_${rel}> "${TTYD_REPO_DIR}/tools/port3ds/rel_link.py"
        COMMAND_EXPAND_LISTS
        COMMENT "Linking static REL ${rel}")
    list(APPEND TTYD_REL_OBJECTS "${_rel_obj}")
    string(APPEND _rel_decls
        "extern void ttyd_rel_${rel}_prolog(void), ttyd_rel_${rel}_epilog(void), ttyd_rel_${rel}_unresolved(void);\n"
        "extern u8 __ttyd_rel_${rel}_data_start[], __ttyd_rel_${rel}_data_end[];\n"
        "extern u8 __ttyd_rel_${rel}_bss_start[], __ttyd_rel_${rel}_bss_end[];\n")
    string(APPEND _rel_rows
        "    {\"${rel}\", ttyd_rel_${rel}_prolog, ttyd_rel_${rel}_epilog, ttyd_rel_${rel}_unresolved,\n"
        "     __ttyd_rel_${rel}_data_start, __ttyd_rel_${rel}_data_end, __ttyd_rel_${rel}_bss_start, __ttyd_rel_${rel}_bss_end},\n")
endforeach()
file(WRITE "${TTYD_GEN_DIR}/rel_table.c.tmp"
    "/* Generated by port3ds/CMakeLists.txt: the RELs linked into this build. */\n"
    "#include \"port3ds/rel_static.h\"\n\n${_rel_decls}\n"
    "const PortRelStatic port_rel_table[] = {\n${_rel_rows}};\n"
    "const int port_rel_count = sizeof(port_rel_table) / sizeof(port_rel_table[0]);\n")
configure_file("${TTYD_GEN_DIR}/rel_table.c.tmp" "${TTYD_GEN_DIR}/rel_table.c" COPYONLY)
file(WRITE "${TTYD_GEN_DIR}/port3ds_build.h.tmp"
    "#define PORT3DS_BUILD_STRING \"#${TTYD_BUILD_COUNT} ${TTYD_BUILD_HASH}\"\n")
configure_file("${TTYD_GEN_DIR}/port3ds_build.h.tmp" "${TTYD_GEN_DIR}/port3ds_build.h" COPYONLY)
add_custom_target(ttyd_rels DEPENDS ${TTYD_REL_OBJECTS})

# The port, in two halves with different enum ABIs (see docs/PORT_3DS_PLAN.md):
#   port3ds/game/  the SDK replacement the game calls (OS, DVD, CARD, PAD, VI,
#                  GX front end, MTX, MusyX hardware front end). Built like
#                  game code: int-sized enums. Includes game and SDK headers,
#                  never libctru/citro3d headers.
#   port3ds/plat/  everything that talks to libctru, citro3d and the system
#                  (boot, threads, files, input, GPU back end, NDSP, log,
#                  crash handler, debug menu). Built with devkitARM's short
#                  enums, the ABI libctru and citro3d were built with.
#                  Includes libctru headers, never game headers.
# The two halves talk only through port3ds/include/plat/*.h, whose types are
# fixed-size integers, pointers and structs of those (no enum-typed fields).
set(TTYD_PORT_WARNINGS -Wall -Wextra -Wno-unused-parameter)
file(GLOB_RECURSE TTYD_PORT_GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/game/*.c")
file(GLOB_RECURSE TTYD_PORT_PLAT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/plat/*.c")

# Matrix library: the SDK's own C_ functions, with the PS names on top.
add_custom_command(
    OUTPUT "${TTYD_GEN_DIR}/mtx_sdk.c"
    COMMAND "${Python3_EXECUTABLE}" -I "${TTYD_REPO_DIR}/tools/port3ds/gen_mtx.py" "${TTYD_GEN_DIR}/mtx_sdk.c"
    DEPENDS "${TTYD_REPO_DIR}/tools/port3ds/gen_mtx.py"
    COMMENT "Generating the matrix library")
# SDK sources compiled unchanged: the heap allocator.
set(TTYD_SDK_SOURCES
    "${TTYD_REPO_DIR}/libs/dolsdk2004/src/os/OSAlloc.c"
    "${TTYD_REPO_DIR}/libs/dolsdk2004/src/pad/Padclamp.c")
add_library(ttyd_port_game OBJECT ${TTYD_PORT_GAME_SOURCES} ${TTYD_SDK_SOURCES}
    "${TTYD_GEN_DIR}/rel_table.c" "${TTYD_GEN_DIR}/mtx_sdk.c")
set_source_files_properties(${TTYD_SDK_SOURCES} "${TTYD_GEN_DIR}/mtx_sdk.c" TARGET_DIRECTORY ttyd_port_game
    PROPERTIES COMPILE_OPTIONS "-w")
target_compile_options(ttyd_port_game PRIVATE -std=gnu11 -O2 -g -fno-short-enums ${TTYD_PORT_WARNINGS}
    -fno-strict-aliasing -fwrapv -ffp-contract=off -ffunction-sections -fdata-sections
    -include "${CMAKE_CURRENT_LIST_DIR}/include/port3ds_prefix.h")
target_compile_definitions(ttyd_port_game PRIVATE ${TTYD_DEFINES} PORT3DS_GAME_SIDE=1)
target_include_directories(ttyd_port_game PRIVATE ${TTYD_INCLUDES})

add_library(ttyd_port_plat OBJECT ${TTYD_PORT_PLAT_SOURCES})
target_compile_options(ttyd_port_plat PRIVATE -std=gnu11 -O2 -g ${TTYD_PORT_WARNINGS}
    -fno-strict-aliasing -ffunction-sections -fdata-sections)
target_compile_definitions(ttyd_port_plat PRIVATE PORT3DS_PLAT_SIDE=1)
target_include_directories(ttyd_port_plat PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include" "${TTYD_GEN_DIR}")

add_executable(ttyd
    $<TARGET_OBJECTS:ttyd_dol>
    $<TARGET_OBJECTS:ttyd_musyx>
    $<TARGET_OBJECTS:ttyd_port_game>
    $<TARGET_OBJECTS:ttyd_port_plat>
    ${TTYD_REL_OBJECTS})
add_dependencies(ttyd ttyd_rels)
# --no-enum-size-warning: the two enum ABIs are mixed on purpose (see above).
target_link_options(ttyd PRIVATE -Wl,--gc-sections -Wl,--no-enum-size-warning
    -Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/ttyd.map)
target_link_libraries(ttyd PRIVATE citro3d ctru m)

ttyd_add_cia(ttyd
    TITLE "TTYD 3DS"
    SUBTITLE "PORT"
    SHORT_DESC "Paper Mario TTYD (3DS port)"
    UNIQUE_ID 0xF7D10
    PRODUCT_CODE CTR-H-TTYD)
