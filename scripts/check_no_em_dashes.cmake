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

# python3 on macOS/Linux; python on Windows (the python.org installer ships
# python.exe, not always python3.exe). Try python3 first so modern macOS --
# which has no bare `python` -- still finds an interpreter.
find_program(LOCUS_PYTHON NAMES python3 python)
if(NOT LOCUS_PYTHON)
    message(FATAL_ERROR "S5.P lint: no python3/python interpreter found on PATH")
endif()

execute_process(
    COMMAND "${LOCUS_PYTHON}" "${PY_SCRIPT}" "${SOURCE_DIR}" "${ALLOWLIST}"
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
