# Run at every build (port3ds/CMakeLists.txt, target ttyd_build_stamp): writes
# the build number into port3ds_build.h, so the log, HUD and crash reports
# show the commit being built, not the commit that was checked out when CMake
# last configured. configure_file leaves the file alone when it is unchanged,
# so the game objects only rebuild when the commit changes.
#
# Also writes PORT3DS_VERSION: port3ds/VERSION, plus "-dev+<shorthash>" unless
# the tree is clean and HEAD is tagged v<version> (docs/RELEASE.md).
#
# Inputs: -DTTYD_REPO_DIR, -DTTYD_GEN_DIR, -DGIT_EXECUTABLE (may be empty),
# -DTTYD_VERSION_CORE.

set(_count 0)
set(_hash unknown)
if(GIT_EXECUTABLE)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-list --count HEAD WORKING_DIRECTORY "${TTYD_REPO_DIR}"
        OUTPUT_VARIABLE _count OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD WORKING_DIRECTORY "${TTYD_REPO_DIR}"
        OUTPUT_VARIABLE _hash OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(_count STREQUAL "")
    set(_count 0)
endif()
if(_hash STREQUAL "")
    set(_hash unknown)
endif()
set(_version "${TTYD_VERSION_CORE}")
set(_release FALSE)
if(GIT_EXECUTABLE)
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --exact-match --tags --match "v${TTYD_VERSION_CORE}" HEAD
        WORKING_DIRECTORY "${TTYD_REPO_DIR}" RESULT_VARIABLE _tag_rc OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
        WORKING_DIRECTORY "${TTYD_REPO_DIR}" OUTPUT_VARIABLE _dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_tag_rc EQUAL 0 AND _dirty STREQUAL "")
        set(_release TRUE)
    endif()
endif()
if(NOT _release)
    set(_version "${_version}-dev+${_hash}")
endif()
file(WRITE "${TTYD_GEN_DIR}/port3ds_build.h.tmp"
    "#define PORT3DS_BUILD_STRING \"#${_count} ${_hash}\"\n"
    "#define PORT3DS_VERSION \"${_version}\"\n")
configure_file("${TTYD_GEN_DIR}/port3ds_build.h.tmp" "${TTYD_GEN_DIR}/port3ds_build.h" COPYONLY)
