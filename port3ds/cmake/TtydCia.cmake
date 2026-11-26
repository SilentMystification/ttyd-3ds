# ttyd_add_cia(<target>
#     TITLE <text> [NAME <text>] SUBTITLE <text> UNIQUE_ID <0x...> PRODUCT_CODE <KTR-N-XXXX>
#     [VERSION_MAJOR <n> VERSION_MINOR <n> VERSION_MICRO <n> VERSION_TEXT <x.y.z>]
#     [SHORT_DESC <text>] [AUTHOR <text>])
#
# TITLE is the short name the rsf takes (makerom limits it to 8 characters).
# NAME is the HOME Menu title (SMDH short title), the banner/icon text and
# the default long description; it defaults to TITLE.
#
# Builds <target>.3dsx and <target>.cia next to the ELF of executable
# <target>. The banner, icon and banner sound are generated placeholder art
# (tools/port3ds/make_placeholder_art.py); no game data is involved.
# The CIA uses port3ds/rsf/ttyd.rsf.

option(TTYD_CIA "Build the CIA (makerom compresses the code: ~70-100 s); OFF builds the 3dsx only" ON)
option(TTYD_CIA_DETERMINISTIC "Rewrite the CIA's random ticket/TMD fields for byte-identical builds (console install untested)" OFF)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(MAKEROM makerom)
find_program(BANNERTOOL bannertool)
find_program(SMDHTOOL smdhtool HINTS "$ENV{DEVKITPRO}/tools/bin" REQUIRED)
find_program(TOOL_3DSXTOOL 3dsxtool HINTS "$ENV{DEVKITPRO}/tools/bin" REQUIRED)
if(NOT MAKEROM OR NOT BANNERTOOL)
    message(STATUS "makerom or bannertool not found: building the 3dsx only (the Docker build makes the CIA)")
endif()

set(TTYD_PORT3DS_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(TTYD_PORT3DS_DIR "${TTYD_PORT3DS_DIR}" ABSOLUTE)
get_filename_component(TTYD_REPO_DIR "${TTYD_PORT3DS_DIR}/.." ABSOLUTE)

function(ttyd_add_cia target)
    cmake_parse_arguments(ARG "" "TITLE;NAME;SUBTITLE;UNIQUE_ID;PRODUCT_CODE;SHORT_DESC;AUTHOR;VERSION_MAJOR;VERSION_MINOR;VERSION_MICRO;VERSION_TEXT" "" ${ARGN})
    foreach(req TITLE UNIQUE_ID PRODUCT_CODE)
        if(NOT ARG_${req})
            message(FATAL_ERROR "ttyd_add_cia(${target}): ${req} is required")
        endif()
    endforeach()
    if(NOT ARG_NAME)
        set(ARG_NAME "${ARG_TITLE}")
    endif()
    if(NOT ARG_SHORT_DESC)
        set(ARG_SHORT_DESC "${ARG_NAME}")
    endif()
    # The CIA title version (makerom -major/-minor/-micro) and the version in
    # the SMDH description (the SMDH has no version field).
    set(version_args)
    if(DEFINED ARG_VERSION_MAJOR)
        set(version_args -major ${ARG_VERSION_MAJOR} -minor ${ARG_VERSION_MINOR} -micro ${ARG_VERSION_MICRO})
    endif()
    if(ARG_VERSION_TEXT)
        set(ARG_SHORT_DESC "${ARG_SHORT_DESC} v${ARG_VERSION_TEXT}")
    endif()
    if(NOT ARG_AUTHOR)
        set(ARG_AUTHOR "ttyd-3ds")
    endif()

    set(art "${CMAKE_CURRENT_BINARY_DIR}/${target}_art")
    set(rsf "${TTYD_PORT3DS_DIR}/rsf/ttyd.rsf")
    set(gen "${TTYD_REPO_DIR}/tools/port3ds/make_placeholder_art.py")

    add_custom_command(
        OUTPUT "${art}/banner.png" "${art}/icon.png" "${art}/banner.wav"
        COMMAND "${Python3_EXECUTABLE}" -I "${gen}" --title "${ARG_NAME}" --subtitle "${ARG_SUBTITLE}" --out-dir "${art}"
        DEPENDS "${gen}"
        VERBATIM
        COMMENT "Generating placeholder art for ${target}")

    add_custom_command(
        OUTPUT "${art}/icon.smdh"
        COMMAND "${SMDHTOOL}" --create "${ARG_NAME}" "${ARG_SHORT_DESC}" "${ARG_AUTHOR}" "${art}/icon.png" "${art}/icon.smdh"
        DEPENDS "${art}/icon.png"
        VERBATIM
        COMMENT "Building SMDH for ${target}")

    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}")
    add_custom_command(
        OUTPUT "${out_dir}/${target}.3dsx"
        COMMAND "${TOOL_3DSXTOOL}" "$<TARGET_FILE:${target}>" "${out_dir}/${target}.3dsx" "--smdh=${art}/icon.smdh"
        DEPENDS ${target} "${art}/icon.smdh"
        VERBATIM
        COMMENT "Building ${target}.3dsx")

    if(NOT TTYD_CIA OR NOT MAKEROM OR NOT BANNERTOOL)
        add_custom_target(${target}_package ALL DEPENDS "${out_dir}/${target}.3dsx")
        return()
    endif()

    add_custom_command(
        OUTPUT "${art}/banner.bnr"
        COMMAND "${BANNERTOOL}" makebanner -i "${art}/banner.png" -a "${art}/banner.wav" -o "${art}/banner.bnr"
        DEPENDS "${art}/banner.png" "${art}/banner.wav"
        VERBATIM
        COMMENT "Building banner for ${target}")

    # makerom randomises the ticket title key, ticket ID and TMD content ID: pin them (reproducible CIA) when enabled.
    set(determinize)
    if(TTYD_CIA_DETERMINISTIC)
        set(determinize COMMAND "${Python3_EXECUTABLE}" -I "${TTYD_REPO_DIR}/tools/port3ds/cia_deterministic.py" "${out_dir}/${target}.cia")
    endif()

    add_custom_command(
        OUTPUT "${out_dir}/${target}.cia"
        COMMAND "${MAKEROM}" -f cia -o "${out_dir}/${target}.cia" -target t -exefslogo
                -elf "$<TARGET_FILE:${target}>" -rsf "${rsf}"
                ${version_args}
                -icon "${art}/icon.smdh" -banner "${art}/banner.bnr"
                "-DAPP_TITLE=${ARG_TITLE}" "-DAPP_PRODUCT_CODE=${ARG_PRODUCT_CODE}"
                "-DAPP_UNIQUE_ID=${ARG_UNIQUE_ID}"
        ${determinize}
        DEPENDS ${target} "${rsf}" "${art}/icon.smdh" "${art}/banner.bnr"
                "${TTYD_REPO_DIR}/tools/port3ds/cia_deterministic.py"
        VERBATIM
        COMMENT "Building ${target}.cia")

    add_custom_target(${target}_package ALL
        DEPENDS "${out_dir}/${target}.3dsx" "${out_dir}/${target}.cia")
endfunction()
