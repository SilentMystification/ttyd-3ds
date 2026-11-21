cmake_minimum_required(VERSION 3.20)
project(ttyd_probe C)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/TtydCia.cmake")

add_executable(ttyd_probe source/main.c)
target_compile_options(ttyd_probe PRIVATE -O2 -Wall -Wextra -Werror)
target_link_libraries(ttyd_probe PRIVATE ctru m)

ttyd_add_cia(ttyd_probe
    TITLE "TTYD 3DS"
    SUBTITLE "PROBE"
    SHORT_DESC "TTYD 3DS hardware probe"
    UNIQUE_ID 0xF7D1F
    PRODUCT_CODE CTR-H-TTYP)
