# Run at every build (port3ds/CMakeLists.txt, target ttyd_build_stamp): writes
# the build number into port3ds_build.h, so the log, HUD and crash reports
# show the commit being built, not the commit that was checked out when CMake
# last configured. configure_file leaves the file alone when it is unchanged,
# so the game objects only rebuild when the commit changes.
#
# Inputs: -DTTYD_REPO_DIR, -DTTYD_GEN_DIR, -DGIT_EXECUTABLE (may be empty).

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
file(WRITE "${TTYD_GEN_DIR}/port3ds_build.h.tmp"
    "#define PORT3DS_BUILD_STRING \"#${_count} ${_hash}\"\n")
configure_file("${TTYD_GEN_DIR}/port3ds_build.h.tmp" "${TTYD_GEN_DIR}/port3ds_build.h" COPYONLY)
