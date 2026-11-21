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
#   enums stay int-sized (no -fshort-enums), as MWCC -enum int.
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
    -std=gnu99 -O2 -g
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
ttyd_game_objects(ttyd_musyx ${TTYD_MUSYX_SOURCES})
foreach(rel ${TTYD_RELS})
    ttyd_game_objects(ttyd_rel_${rel} ${TTYD_REL_${rel}_SOURCES})
    if(EXISTS "${TTYD_REPO_DIR}/rels/${rel}/include")
        target_include_directories(ttyd_rel_${rel} PRIVATE "${TTYD_REPO_DIR}/rels/${rel}/include")
    endif()
endforeach()
