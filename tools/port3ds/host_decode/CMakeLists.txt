# Host decode tool: the port's disc-format converters, built for the machine you work on.
#
#   cmake -S tools/port3ds/host_decode -B build/host_decode && cmake --build build/host_decode
#   build/host_decode/host_decode --disc iso/game.iso all
#
# This is a separate project from port3ds/ (the 3DS game): it has its own toolchain (the host compiler) and does
# not share a build directory with it. The converter sources are the ones the port is built from; see
# docs/PORT_3DS_HOST_DECODE.md. A game other than TTYD supplies its own table (-DHOST_DECODE_FORMATS=...).
cmake_minimum_required(VERSION 3.20)
project(host_decode C)

set(TTYD_VERSION G8ME01 CACHE STRING "Game version (picks the disc-data table)")
set(HOST_DECODE_FORMATS "${CMAKE_CURRENT_LIST_DIR}/formats_ttyd.c" CACHE FILEPATH "The game's converter table")

get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN}")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(HOST_READELF readelf REQUIRED)

# The byte-swap tables, from the layouts of a 32-bit probe object: the 3DS has 4-byte pointers and 8-byte aligned
# doubles, as -m32 -malign-double does, and the compiler needs no 32-bit C library to compile the probe (it only
# reads declarations; host_prefix.h supplies the types).
set(PROBE_FLAGS -m32 -malign-double -ffreestanding -DHOST_PROBE -std=gnu99 -O0 -g -gdwarf-4
    -fno-eliminate-unused-debug-types -fno-short-enums -fcommon -w
    -include "${CMAKE_CURRENT_LIST_DIR}/host_prefix.h"
    -DVERSION_${TTYD_VERSION} -DBUILD_VERSION=1 -DSDK_REVISION=1 -DNDEBUG
    -I "${CMAKE_CURRENT_LIST_DIR}" -I "${REPO}/port3ds/include" -I "${REPO}/include"
    -I "${REPO}/libs/dolsdk2004/include" -I "${REPO}/libs/musyx/include" -iquote "${REPO}/include/sdk")
add_custom_command(
    OUTPUT "${GEN}/swap_probe.c"
    COMMAND "${Python3_EXECUTABLE}" -I "${REPO}/tools/port3ds/gen_swap.py" probe --out "${GEN}/swap_probe.c"
    DEPENDS "${REPO}/tools/port3ds/gen_swap.py")
add_custom_command(
    OUTPUT "${GEN}/swap_probe.o"
    COMMAND "${CMAKE_C_COMPILER}" ${PROBE_FLAGS} -c "${GEN}/swap_probe.c" -o "${GEN}/swap_probe.o"
    DEPENDS "${GEN}/swap_probe.c" "${CMAKE_CURRENT_LIST_DIR}/host_prefix.h" VERBATIM)
add_custom_command(
    OUTPUT "${GEN}/port_swap_gen.c" "${GEN}/port_swap_gen.h"
    COMMAND "${Python3_EXECUTABLE}" -I "${REPO}/tools/port3ds/gen_swap.py" tables --host
            --readelf "${HOST_READELF}" --obj "${GEN}/swap_probe.o"
            --out-c "${GEN}/port_swap_gen.c" --out-h "${GEN}/port_swap_gen.h"
    DEPENDS "${GEN}/swap_probe.o" "${REPO}/tools/port3ds/gen_swap.py")

# Where this version keeps the data ports read from the disc.
execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${REPO}/tools/port3ds/gen_disc_data.py"
    --version ${TTYD_VERSION} "${GEN}/port_disc_data_gen.h" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "gen_disc_data.py failed")
endif()

# The converters: the same files port3ds/CMakeLists.txt compiles (its GLOB of port3ds/game/*.c, here only the ones
# that convert data and read the image).
set(CONVERTERS
    "${REPO}/port3ds/game/port_swap.c"
    "${REPO}/port3ds/game/musyx_swap.c"
    "${REPO}/port3ds/game/disc_data.c"
    "${REPO}/port3ds/game/fst.c"
    "${REPO}/port3ds/game/formats/tpl.c"
    "${REPO}/port3ds/game/formats/anim.c"
    "${REPO}/port3ds/game/formats/map.c"
    "${REPO}/port3ds/game/formats/font.c"
    "${REPO}/port3ds/game/formats/camroad.c")

add_executable(host_decode
    main.c fst.c shim.c "${HOST_DECODE_FORMATS}" ${CONVERTERS} "${GEN}/port_swap_gen.c")
set_source_files_properties("${GEN}/port_swap_gen.c" "${GEN}/port_swap_gen.h" PROPERTIES GENERATED TRUE)
# Same flags as the port's game-side code, minus the ARM-specific ones.
target_compile_options(host_decode PRIVATE -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter
    -Wno-unknown-pragmas -fno-strict-aliasing -fwrapv -ffp-contract=off
    -include "${CMAKE_CURRENT_LIST_DIR}/host_prefix.h")
target_compile_definitions(host_decode PRIVATE VERSION_${TTYD_VERSION} BUILD_VERSION=1 SDK_REVISION=1 NDEBUG PORT3DS_GAME_SIDE=1
    MUSY_VERSION_MAJOR=2 MUSY_VERSION_MINOR=0 MUSY_VERSION_PATCH=3 MUSY_TARGET=0)
target_include_directories(host_decode PRIVATE "${GEN}" "${CMAKE_CURRENT_LIST_DIR}" "${REPO}/port3ds/include"
    "${REPO}/include" "${REPO}/libs/dolsdk2004/include" "${REPO}/libs/musyx/include")
target_compile_options(host_decode PRIVATE "SHELL:-iquote ${REPO}/include/sdk")
