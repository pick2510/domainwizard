# Run in script mode (cmake -P) by the wrftools_git_version custom target in
# CMakeLists.txt, which is built with every `cmake --build` (not just on
# reconfigure) so the About dialog's commit hash never goes stale - a plain
# execute_process() in CMakeLists.txt at configure time would only refresh
# when something else happened to trigger a reconfigure.
#
# Expected on the command line: SRC_DIR (repo root, for `git` to run in),
# SRC (git_version.hpp.in) and DST (generated header path).

find_package(Git QUIET)

set(WRFTOOLS_GIT_HASH "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${SRC_DIR}
        OUTPUT_VARIABLE _wrftools_git_hash
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _wrftools_git_result
    )
    if(_wrftools_git_result EQUAL 0 AND NOT _wrftools_git_hash STREQUAL "")
        set(WRFTOOLS_GIT_HASH "${_wrftools_git_hash}")
    endif()
endif()

string(TIMESTAMP WRFTOOLS_BUILD_DATE "%Y-%m-%d" UTC)

# configure_file() only rewrites DST if its content actually changed, so an
# unchanged hash/date leaves the generated header's mtime untouched and
# downstream targets are not needlessly recompiled (the About dialog's date
# then only actually changes on the first build of a new UTC day).
configure_file(${SRC} ${DST} @ONLY)
