# TTYD 3DS port: the game build.
#
#   tools/port3ds/build.sh            (Docker, Linux)
#   tools\port3ds\build.ps1           (Docker, Windows)
#
# Compiles the decomp's game code (src/, rels/) and MusyX for the ARM11 with
# the port SDK in port3ds/. See docs/PORT_3DS_PLAN.md.

cmake_minimum_required(VERSION 3.20)
project(ttyd3ds C ASM)

# One build per region (docs/PORT_3DS_PLAN.md, "Regions"): -DREGION=US|JP|PAL,
# or the disc's game ID as -DTTYD_VERSION. Each region has its own sources
# (sources_<version>.cmake), swap tables, disc-data table, REL set and CIA.
set(TTYD_VERSION G8ME01 CACHE STRING "Game version: G8ME01 (US), G8MJ01 (JP) or G8MP01 (PAL)")
set_property(CACHE TTYD_VERSION PROPERTY STRINGS G8ME01 G8MJ01 G8MP01)
set(REGION "" CACHE STRING "Region (US, JP or PAL); sets TTYD_VERSION")
set_property(CACHE REGION PROPERTY STRINGS US JP PAL)
set(TTYD_REGION_VERSION_US G8ME01)
set(TTYD_REGION_VERSION_JP G8MJ01)
set(TTYD_REGION_VERSION_PAL G8MP01)
if(REGION)
    string(TOUPPER "${REGION}" _region)
    if(NOT DEFINED TTYD_REGION_VERSION_${_region})
        message(FATAL_ERROR "REGION must be US, JP or PAL (got ${REGION})")
    endif()
    set(TTYD_VERSION ${TTYD_REGION_VERSION_${_region}} CACHE STRING "" FORCE)
    # applied once: TTYD_VERSION keeps it, so a later -DTTYD_VERSION is not overridden
    set(REGION "" CACHE STRING "Region (US, JP or PAL); sets TTYD_VERSION" FORCE)
endif()
if(NOT TTYD_VERSION MATCHES "^(G8ME01|G8MJ01|G8MP01)$")
    message(FATAL_ERROR "TTYD_VERSION must be G8ME01, G8MJ01 or G8MP01 (got ${TTYD_VERSION})")
endif()
# The CIA identity of each region. The probe is 0xFD887 (KTR-N-TTYP) and the
# ctr-devkit examples 0xFD888 and 0xFD889, so PAL takes KTR-N-TTYE.
set(TTYD_CIA_G8ME01 0xFD886 KTR-N-TTYD US)
set(TTYD_CIA_G8MJ01 0xFD88A KTR-N-TTYJ JP)
set(TTYD_CIA_G8MP01 0xFD88B KTR-N-TTYE PAL)
list(GET TTYD_CIA_${TTYD_VERSION} 0 TTYD_UNIQUE_ID)
list(GET TTYD_CIA_${TTYD_VERSION} 1 TTYD_PRODUCT_CODE)
list(GET TTYD_CIA_${TTYD_VERSION} 2 TTYD_REGION_NAME)
message(STATUS "TTYD 3DS: ${TTYD_REGION_NAME} (${TTYD_VERSION}), CIA ${TTYD_UNIQUE_ID} ${TTYD_PRODUCT_CODE}")

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
    # The sources are UTF-8; the game's strings are Shift-JIS, as the
    # matching build makes them (sjiswrap): names it compares with the
    # disc's files (NPC tribes in setup files, messages) must match.
    -fexec-charset=CP932
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
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
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
# Where this version keeps the data ports read from the disc (port_disc_data.h),
# and its GameCube arena. The arena's high end comes from the disc itself:
# TTYD_DISC names the image, else iso/<version>.iso or iso/game.iso is used
# when it is this version's disc. Without it the generator uses the G8ME01
# value (exact for G8ME01, a stand-in for the others; see gen_disc_data.py).
set(TTYD_DISC "" CACHE FILEPATH "This version's disc image, for the generators that read the disc (optional)")
set(_disc_args)
if(TTYD_DISC)
    set(_disc_args --disc "${TTYD_DISC}")
else()
    foreach(_cand "${TTYD_REPO_DIR}/iso/${TTYD_VERSION}.iso" "${TTYD_REPO_DIR}/iso/game.iso")
        if(EXISTS "${_cand}" AND NOT _disc_args)
            file(READ "${_cand}" _id LIMIT 6)
            if(_id STREQUAL TTYD_VERSION)
                set(_disc_args --disc "${_cand}")
            endif()
        endif()
    endforeach()
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${TTYD_REPO_DIR}/tools/port3ds/gen_disc_data.py"
    --version ${TTYD_VERSION} ${_disc_args} "${TTYD_GEN_DIR}/port_disc_data_gen.h.tmp" RESULT_VARIABLE _gen_rc)
if(NOT _gen_rc EQUAL 0)
    message(FATAL_ERROR "gen_disc_data.py failed")
endif()
configure_file("${TTYD_GEN_DIR}/port_disc_data_gen.h.tmp" "${TTYD_GEN_DIR}/port_disc_data_gen.h" COPYONLY)
file(WRITE "${TTYD_GEN_DIR}/port3ds_build.h.tmp"
    "#define PORT3DS_BUILD_STRING \"#${TTYD_BUILD_COUNT} ${TTYD_BUILD_HASH}\"\n")
configure_file("${TTYD_GEN_DIR}/port3ds_build.h.tmp" "${TTYD_GEN_DIR}/port3ds_build.h" COPYONLY)
# Refreshes port3ds_build.h on every build (see cmake/BuildStamp.cmake).
add_custom_target(ttyd_build_stamp
    COMMAND "${CMAKE_COMMAND}" "-DTTYD_REPO_DIR=${TTYD_REPO_DIR}" "-DTTYD_GEN_DIR=${TTYD_GEN_DIR}"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}" -P "${CMAKE_CURRENT_LIST_DIR}/cmake/BuildStamp.cmake"
    VERBATIM)
add_custom_target(ttyd_rels DEPENDS ${TTYD_REL_OBJECTS})

# Byte-swap tables for the disc formats (port3ds/game/formats/), from the
# port's own debug info: a probe object names each format's structs, and
# tools/port3ds/gen_swap.py reads their layouts from its DWARF.
execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${TTYD_REPO_DIR}/tools/port3ds/gen_swap.py" probe --game
    --out "${TTYD_GEN_DIR}/swap_probe.c.tmp" RESULT_VARIABLE _gen_rc)
if(NOT _gen_rc EQUAL 0)
    message(FATAL_ERROR "gen_swap.py probe failed")
endif()
configure_file("${TTYD_GEN_DIR}/swap_probe.c.tmp" "${TTYD_GEN_DIR}/swap_probe.c" COPYONLY)
ttyd_game_objects(ttyd_swap_probe "${TTYD_GEN_DIR}/swap_probe.c")
target_compile_options(ttyd_swap_probe PRIVATE -O0 -gdwarf-4 -fno-eliminate-unused-debug-types)
if(CMAKE_READELF)
    set(TTYD_READELF "${CMAKE_READELF}")
else()
    string(REGEX REPLACE "objcopy([^/]*)$" "readelf\\1" TTYD_READELF "${CMAKE_OBJCOPY}")
endif()
add_custom_command(
    OUTPUT "${TTYD_GEN_DIR}/port_swap_gen.c" "${TTYD_GEN_DIR}/port_swap_gen.h"
    COMMAND "${Python3_EXECUTABLE}" -I "${TTYD_REPO_DIR}/tools/port3ds/gen_swap.py" tables
            --readelf "${TTYD_READELF}" --obj $<TARGET_OBJECTS:ttyd_swap_probe>
            --out-c "${TTYD_GEN_DIR}/port_swap_gen.c" --out-h "${TTYD_GEN_DIR}/port_swap_gen.h"
    DEPENDS ttyd_swap_probe $<TARGET_OBJECTS:ttyd_swap_probe> "${TTYD_REPO_DIR}/tools/port3ds/gen_swap.py"
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMENT "Generating the disc formats' byte-swap tables")

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
set(TTYD_PORT_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-unknown-pragmas)
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
    "${TTYD_REPO_DIR}/libs/dolsdk2004/src/pad/Padclamp.c"
    "${TTYD_REPO_DIR}/libs/dolsdk2004/src/gx/GXDraw.c")
add_library(ttyd_port_game OBJECT ${TTYD_PORT_GAME_SOURCES} ${TTYD_SDK_SOURCES}
    "${TTYD_GEN_DIR}/rel_table.c" "${TTYD_GEN_DIR}/mtx_sdk.c"
    "${TTYD_GEN_DIR}/port_swap_gen.c" "${TTYD_GEN_DIR}/port_swap_gen.h")
set_source_files_properties(${TTYD_SDK_SOURCES} "${TTYD_GEN_DIR}/mtx_sdk.c" TARGET_DIRECTORY ttyd_port_game
    PROPERTIES COMPILE_OPTIONS "-w")
# GXDraw.c has a global named index, which newlib declares as a function
# outside strict ISO C.
set_source_files_properties("${TTYD_REPO_DIR}/libs/dolsdk2004/src/gx/GXDraw.c" TARGET_DIRECTORY ttyd_port_game
    PROPERTIES COMPILE_OPTIONS "-w;-std=c99")
# MusyX's bool is a 4-byte typedef only up to C99 (musyx.h); the MusyX
# hardware layer is compiled as MusyX is.
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/game/audio.c" TARGET_DIRECTORY ttyd_port_game
    PROPERTIES COMPILE_OPTIONS "-std=gnu99")
target_compile_options(ttyd_port_game PRIVATE -std=gnu11 -O2 -g -fno-short-enums ${TTYD_PORT_WARNINGS}
    -fno-strict-aliasing -fwrapv -ffp-contract=off -ffunction-sections -fdata-sections
    -include "${CMAKE_CURRENT_LIST_DIR}/include/port3ds_prefix.h")
target_compile_definitions(ttyd_port_game PRIVATE ${TTYD_DEFINES} PORT3DS_GAME_SIDE=1)
target_include_directories(ttyd_port_game PRIVATE ${TTYD_INCLUDES} "${TTYD_REPO_DIR}/libs/dolsdk2004/src/gx")

# The GX vertex shader (port3ds/shaders/gx.v.pica), embedded as gx_shbin.
ctr_add_shader_library(ttyd_gx_shader OUTPUT gx.shbin "${CMAKE_CURRENT_LIST_DIR}/shaders/gx.v.pica")
dkp_add_embedded_binary_library(ttyd_gx_shader_bin ttyd_gx_shader)
# Output-register discipline (each output written once, in full, outside
# branches; no b15): the PICA200 hangs otherwise, Azahar does not.
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gx_vsh_check.stamp"
    COMMAND "${Python3_EXECUTABLE}" -I "${TTYD_REPO_DIR}/tools/port3ds/check_vsh.py"
            "${CMAKE_CURRENT_LIST_DIR}/shaders/gx.v.pica" --stamp "${CMAKE_CURRENT_BINARY_DIR}/gx_vsh_check.stamp"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/shaders/gx.v.pica" "${TTYD_REPO_DIR}/tools/port3ds/check_vsh.py"
    VERBATIM)
add_custom_target(ttyd_gx_shader_check DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/gx_vsh_check.stamp")
add_dependencies(ttyd_gx_shader ttyd_gx_shader_check)

# ctr-devkit (libs/ctr-devkit): the crash handler (plat/crash.c) and the updater (plat/devupdate.c).
add_subdirectory("${TTYD_REPO_DIR}/libs/ctr-devkit" ctr-devkit)

add_library(ttyd_port_plat OBJECT ${TTYD_PORT_PLAT_SOURCES})
add_dependencies(ttyd_port_plat ttyd_build_stamp)
add_dependencies(ttyd_port_game ttyd_build_stamp)
add_dependencies(ttyd_dol ttyd_build_stamp)
target_link_libraries(ttyd_port_plat PRIVATE ttyd_gx_shader_bin)
target_compile_options(ttyd_port_plat PRIVATE -std=gnu11 -O2 -g ${TTYD_PORT_WARNINGS}
    -fno-strict-aliasing -ffunction-sections -fdata-sections)
target_compile_definitions(ttyd_port_plat PRIVATE PORT3DS_PLAT_SIDE=1)
target_include_directories(ttyd_port_plat PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include" "${TTYD_GEN_DIR}"
    "${TTYD_REPO_DIR}/libs/ctr-devkit/include")

add_executable(ttyd
    $<TARGET_OBJECTS:ttyd_dol>
    $<TARGET_OBJECTS:ttyd_musyx>
    $<TARGET_OBJECTS:ttyd_port_game>
    $<TARGET_OBJECTS:ttyd_port_plat>
    ${TTYD_REL_OBJECTS})
add_dependencies(ttyd ttyd_rels)
# --no-enum-size-warning: the two enum ABIs are mixed on purpose (see above).
# --wrap=svcBreak: the crash library turns every svcBreak (libctru and citro3d panics too) into a crash report
target_link_options(ttyd PRIVATE -Wl,--gc-sections -Wl,--wrap=svcBreak -Wl,--no-enum-size-warning
    -Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/ttyd.map)
target_link_libraries(ttyd PRIVATE ttyd_gx_shader_bin ctrdk_devupdate ctrdk_crash citro3d ctru m)

if(TTYD_VERSION STREQUAL G8ME01)
    set(_subtitle "PORT")
    set(_desc "Paper Mario: The Thousand Year Door (3DS port)")
else()
    set(_subtitle "PORT ${TTYD_REGION_NAME}")
    set(_desc "Paper Mario: The Thousand Year Door (3DS port, ${TTYD_REGION_NAME})")
endif()
ttyd_add_cia(ttyd
    TITLE "TTYD 3DS"
    NAME "Paper Mario: The Thousand Year Door"
    SUBTITLE "${_subtitle}"
    SHORT_DESC "${_desc}"
    UNIQUE_ID ${TTYD_UNIQUE_ID}
    PRODUCT_CODE ${TTYD_PRODUCT_CODE})
