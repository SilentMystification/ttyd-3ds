# ttyd_add_cia(<target>
#     TITLE <text> SUBTITLE <text> UNIQUE_ID <0x...> PRODUCT_CODE <CTR-H-XXXX>
#     [SHORT_DESC <text>] [AUTHOR <text>])
#
# Builds <target>.3dsx and <target>.cia next to the ELF of executable
# <target>. The banner, icon and banner sound are generated placeholder art
# (tools/port3ds/make_placeholder_art.py); no game data is involved.
# The CIA uses port3ds/rsf/ttyd.rsf.

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(MAKEROM makerom REQUIRED)
find_program(BANNERTOOL bannertool REQUIRED)

set(TTYD_PORT3DS_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(TTYD_PORT3DS_DIR "${TTYD_PORT3DS_DIR}" ABSOLUTE)
get_filename_component(TTYD_REPO_DIR "${TTYD_PORT3DS_DIR}/.." ABSOLUTE)

function(ttyd_add_cia target)
    cmake_parse_arguments(ARG "" "TITLE;SUBTITLE;UNIQUE_ID;PRODUCT_CODE;SHORT_DESC;AUTHOR" "" ${ARGN})
    foreach(req TITLE UNIQUE_ID PRODUCT_CODE)
        if(NOT ARG_${req})
            message(FATAL_ERROR "ttyd_add_cia(${target}): ${req} is required")
        endif()
    endforeach()
    if(NOT ARG_SHORT_DESC)
        set(ARG_SHORT_DESC "${ARG_TITLE}")
    endif()
    if(NOT ARG_AUTHOR)
        set(ARG_AUTHOR "ttyd-3ds")
    endif()

    set(art "${CMAKE_CURRENT_BINARY_DIR}/${target}_art")
    set(rsf "${TTYD_PORT3DS_DIR}/rsf/ttyd.rsf")
    set(gen "${TTYD_REPO_DIR}/tools/port3ds/make_placeholder_art.py")

    add_custom_command(
        OUTPUT "${art}/banner.png" "${art}/icon.png" "${art}/banner.wav"
        COMMAND "${Python3_EXECUTABLE}" -I "${gen}" --title "${ARG_TITLE}" --subtitle "${ARG_SUBTITLE}" --out-dir "${art}"
        DEPENDS "${gen}"
        COMMENT "Generating placeholder art for ${target}")

    add_custom_command(
        OUTPUT "${art}/banner.bnr"
        COMMAND "${BANNERTOOL}" makebanner -i "${art}/banner.png" -a "${art}/banner.wav" -o "${art}/banner.bnr"
        DEPENDS "${art}/banner.png" "${art}/banner.wav"
        COMMENT "Building banner for ${target}")

    add_custom_command(
        OUTPUT "${art}/icon.smdh"
        COMMAND "${BANNERTOOL}" makesmdh -s "${ARG_TITLE}" -l "${ARG_SHORT_DESC}" -p "${ARG_AUTHOR}"
                -i "${art}/icon.png" -o "${art}/icon.smdh"
        DEPENDS "${art}/icon.png"
        COMMENT "Building SMDH for ${target}")

    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}")
    add_custom_command(
        OUTPUT "${out_dir}/${target}.3dsx"
        COMMAND 3dsxtool "$<TARGET_FILE:${target}>" "${out_dir}/${target}.3dsx" --smdh="${art}/icon.smdh"
        DEPENDS ${target} "${art}/icon.smdh"
        COMMENT "Building ${target}.3dsx")

    add_custom_command(
        OUTPUT "${out_dir}/${target}.cia"
        COMMAND "${MAKEROM}" -f cia -o "${out_dir}/${target}.cia" -target t -exefslogo
                -elf "$<TARGET_FILE:${target}>" -rsf "${rsf}"
                -icon "${art}/icon.smdh" -banner "${art}/banner.bnr"
                "-DAPP_TITLE=${ARG_TITLE}" "-DAPP_PRODUCT_CODE=${ARG_PRODUCT_CODE}"
                "-DAPP_UNIQUE_ID=${ARG_UNIQUE_ID}"
        DEPENDS ${target} "${rsf}" "${art}/icon.smdh" "${art}/banner.bnr"
        COMMENT "Building ${target}.cia")

    add_custom_target(${target}_package ALL
        DEPENDS "${out_dir}/${target}.3dsx" "${out_dir}/${target}.cia")
endfunction()
