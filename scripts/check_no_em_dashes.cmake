# check_no_em_dashes.cmake -- build-time source-encoding lint (S5.P).
#
# Invoked as a POST_BUILD step on locus_core:
#   cmake -DSOURCE_DIR=<root> -DALLOWLIST=<path> -P check_no_em_dashes.cmake
#
# Delegates to check_no_em_dashes.py (same directory) via the system Python.
# Using Python avoids the byte-alignment false-positives that a pure CMake
# regex approach on the HEX-encoded file content produces (CMake's `MATCHES`
# does substring matching, so a pattern like "e28692" can spuriously match
# across nibble boundaries of adjacent ASCII bytes such as ">(i)" = 3e 28 69).
#
# Allow-list format (ALLOWLIST file):
#   Lines starting with '#' are comments.
#   <file_path>:<U+XXXX>  -- exempt one codepoint in one file
#   <file_path>           -- exempt all non-ASCII in that file (bare path)
# Path separators are normalised to '/'.

cmake_minimum_required(VERSION 3.20)

# Locate the Python script next to this CMake script
get_filename_component(SCRIPT_DIR "${CMAKE_SCRIPT_MODE_FILE}" DIRECTORY)
set(PY_SCRIPT "${SCRIPT_DIR}/check_no_em_dashes.py")

if(NOT EXISTS "${PY_SCRIPT}")
    message(FATAL_ERROR "S5.P lint: Python script not found at ${PY_SCRIPT}")
endif()

execute_process(
    COMMAND python "${PY_SCRIPT}" "${SOURCE_DIR}" "${ALLOWLIST}"
    RESULT_VARIABLE py_result
    OUTPUT_VARIABLE py_output
    ERROR_VARIABLE  py_error
)

if(py_output)
    message(STATUS "${py_output}")
endif()
if(py_error)
    message(STATUS "${py_error}")
endif()

if(NOT py_result EQUAL 0)
    message(FATAL_ERROR "S5.P encoding lint failed -- see output above.")
endif()
