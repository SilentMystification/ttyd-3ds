# Times the THP decoder on the 3DS (Azahar or a console): one embedded video frame, decoded on one core and
# with the picture stage on a second core. Not part of the game.
#
#   tools/port3ds/build.sh tools/port3ds/thp_test/arm_bench -DTHP_FRAME=/work/<path to a THP video frame>
#
# THP_FRAME: a file holding one video frame (the JPEG without byte stuffing), for example one cut out of a
# make_thp.py file by extract_frame.py. The path is as the build container sees it (the repository is /work).
cmake_minimum_required(VERSION 3.20)
project(thp_bench C)

include("${CMAKE_CURRENT_LIST_DIR}/../../../../port3ds/cmake/TtydCia.cmake")

if(NOT THP_FRAME)
    message(FATAL_ERROR "pass -DTHP_FRAME=<frame file>")
endif()
file(READ "${THP_FRAME}" frame_hex HEX)
string(LENGTH "${frame_hex}" hex_len)
math(EXPR frame_len "${hex_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," frame_c "${frame_hex}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/frame.h"
    "static const unsigned char frame_data[${frame_len}] = {${frame_c}};\n")

set(AURORA "${CMAKE_CURRENT_LIST_DIR}/../../../../port3ds/third_party/aurora_thp")
add_executable(thp_bench main.c "${AURORA}/thp_video.c")
target_include_directories(thp_bench PRIVATE "${AURORA}" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(thp_bench PRIVATE -O2 -Wall -Wextra -ffp-contract=off)
target_link_libraries(thp_bench PRIVATE ctru m)

ttyd_add_cia(thp_bench
    TITLE "THPBENCH"
    SUBTITLE "THP BENCH"
    SHORT_DESC "THP decoder timing"
    UNIQUE_ID 0xFD8A7
    PRODUCT_CODE KTR-N-TTHB)
