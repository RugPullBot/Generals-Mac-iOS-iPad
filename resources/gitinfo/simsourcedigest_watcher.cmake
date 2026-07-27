# simsourcedigest_watcher.cmake
#
# GeneralsX @feature Claude 27/07/2026
# Build-time 32-bit digest of the simulation-relevant source tree, used by the
# join-time SimID compatibility check (Common/Diagnostic/SimulationId.h).
#
# Structured exactly like its sibling gitinfo/git_watcher.cmake, and for the same
# reason: this file is invoked twice, once at configure time (to register the custom
# target) and once per build via `cmake -P`. In script mode CMake starts a fresh
# process in which CMAKE_SOURCE_DIR is the *invoking process's working directory* -
# for Ninja that is ${sourceDir}/build/<preset>/resources, not the repo root - and no
# find_package(Git) has run, so GIT_EXECUTABLE is empty. Every value the build-time
# half needs is therefore forwarded with -D on the custom command, and nothing below
# Main() may reference CMAKE_SOURCE_DIR or CMAKE_CURRENT_BINARY_DIR.
#
# WINDOWS HOOK (not solved here; Apple targets only in this pass):
#   A Windows clone made with Git for Windows' installer default core.autocrlf=true
#   has a CRLF working tree. `git hash-object` applies the checkin filter by default,
#   so under plain autocrlf a re-saved file still hashes to the committed LF blob and
#   the "only record it if the sha differs" rule below discards it. Do NOT add a root
#   .gitattributes containing `* -text`: -text DISABLES that filter and would make
#   every file in a CRLF working tree land in the overlay, denying every Windows join
#   for a semantically identical tree. If a .gitattributes is wanted, it must be
#   `* text=auto eol=lf` together with a one-time `git add --renormalize .`.

# Short hand for converting paths to absolute.
macro(PATH_TO_ABSOLUTE var_name)
    get_filename_component(${var_name} "${${var_name}}" ABSOLUTE)
endmacro()

# Check that a required variable is set.
macro(CHECK_REQUIRED_VARIABLE var_name)
    if(NOT DEFINED ${var_name})
        message(FATAL_ERROR "The \"${var_name}\" variable must be defined.")
    endif()
    PATH_TO_ABSOLUTE(${var_name})
endmacro()

# Check that an optional variable is set, or, set it to a default value.
macro(CHECK_OPTIONAL_VARIABLE var_name default_value)
    if(NOT DEFINED ${var_name})
        set(${var_name} ${default_value})
    endif()
    PATH_TO_ABSOLUTE(${var_name})
endmacro()

CHECK_REQUIRED_VARIABLE(SIMDIGEST_PRE_CONFIGURE_FILE)
CHECK_REQUIRED_VARIABLE(SIMDIGEST_POST_CONFIGURE_FILE)
CHECK_OPTIONAL_VARIABLE(SIMDIGEST_WORKING_DIR "${CMAKE_SOURCE_DIR}")

if(NOT DEFINED GIT_EXECUTABLE)
    find_package(Git QUIET)
endif()

# ---------------------------------------------------------------------------
# Directory pathspecs only; the extension filter is applied afterwards in CMake,
# NOT as a git pathspec. `git ls-tree` does not run the full pathspec engine - a
# bare '*.cpp' term there matches zero files - and mixing a directory term with a
# glob term ORs them rather than intersecting. Filtering the output lines gives the
# intended intersection and applies identically to ls-tree, diff-index and ls-files,
# so untracked debris such as an editor backup cannot move the digest.
# ---------------------------------------------------------------------------
set(SIMDIGEST_CODE_EXT_REGEX "\\.(c|cc|cpp|cxx|h|hh|hpp|inl)$")
set(SIMDIGEST_BUILD_EXT_REGEX "\\.(cmake|json|in|ini|py|txt)$")

set(SIMDIGEST_PATHS_CODE_SHARED
    Core/GameEngine/Include/Common
    Core/GameEngine/Include/GameLogic
    Core/GameEngine/Include/GameNetwork
    Core/GameEngine/Source/Common
    Core/GameEngine/Source/GameLogic
    Core/GameEngine/Source/GameNetwork
    # Core/Libraries/Include is where this fork's own documented ungated divergences
    # live: Lib/BaseType.h REAL_TO_INT and the Lib/trig.h Sqrt gateway.
    Core/Libraries/Include
    Core/Libraries/Source/WWVegas/WWMath
)

# Build configuration is part of the simulation identity: a flag change can move
# code generation without moving a line of engine source.
set(SIMDIGEST_PATHS_BUILD
    cmake
    triplets
    CMakePresets.json
    vcpkg.json
    vcpkg-lock.json
)

set(SIMDIGEST_PATHS_CODE_ZH
    GeneralsMD/Code/GameEngine/Include/Common
    GeneralsMD/Code/GameEngine/Include/GameLogic
    GeneralsMD/Code/GameEngine/Include/GameNetwork
    GeneralsMD/Code/GameEngine/Source/Common
    GeneralsMD/Code/GameEngine/Source/GameLogic
    GeneralsMD/Code/GameEngine/Source/GameNetwork
)

set(SIMDIGEST_PATHS_CODE_G
    Generals/Code/GameEngine/Include/Common
    Generals/Code/GameEngine/Include/GameLogic
    Generals/Code/GameEngine/Include/GameNetwork
    Generals/Code/GameEngine/Source/Common
    Generals/Code/GameEngine/Source/GameLogic
    Generals/Code/GameEngine/Source/GameNetwork
)

# A git *failure* must never look like a git *absence*: absence yields the reserved 0
# sentinel, failure stops the build. Without this, a broken git (an xcrun shim after a
# macOS update, or "dubious ownership" in a container) makes all three plumbing calls
# print nothing while still exiting non-zero, the tree text is empty, and the digest
# becomes a fixed non-zero constant identical on every machine in that state - a
# silent permanent "compatible".
macro(SimDigestGit out_var)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -c core.quotepath=off ${ARGN}
        WORKING_DIRECTORY "${SIMDIGEST_WORKING_DIR}"
        RESULT_VARIABLE _sd_exit
        OUTPUT_VARIABLE ${out_var}
        ERROR_VARIABLE _sd_err
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _sd_exit EQUAL 0)
        message(FATAL_ERROR
            "SimID: git is present but 'git ${ARGN}' failed (exit ${_sd_exit}) in "
            "${SIMDIGEST_WORKING_DIR}:\n${_sd_err}\n"
            "Refusing to emit a fake source digest - two broken builds would then "
            "report the same value and be allowed to play. Fix git, or configure "
            "with -DSIMDIGEST_ALLOW_NO_GIT=ON to build with the 'unknown provenance' "
            "sentinel instead.")
    endif()
endmacro()

function(SimDigestCollect _out_text _regex)
    set(_paths ${ARGN})

    SimDigestGit(_tree ls-tree -r --full-tree HEAD -- ${_paths})
    string(REPLACE "\n" ";" _tree_lines "${_tree}")
    list(FILTER _tree_lines INCLUDE REGEX "${_regex}")
    list(SORT _tree_lines)

    if(_tree_lines STREQUAL "")
        message(FATAL_ERROR
            "SimID: the path list matched no tracked files under ${SIMDIGEST_WORKING_DIR}. "
            "The digest would be a fixed constant on every machine. Path list: ${_paths}")
    endif()

    SimDigestGit(_changed diff-index --name-only HEAD -- ${_paths})
    SimDigestGit(_untracked ls-files --others --exclude-standard -- ${_paths})

    set(_dirty "")
    if(NOT _changed STREQUAL "")
        string(REPLACE "\n" ";" _c "${_changed}")
        list(APPEND _dirty ${_c})
    endif()
    if(NOT _untracked STREQUAL "")
        string(REPLACE "\n" ";" _u "${_untracked}")
        list(APPEND _dirty ${_u})
    endif()
    if(NOT _dirty STREQUAL "")
        list(FILTER _dirty INCLUDE REGEX "${_regex}")
        list(REMOVE_DUPLICATES _dirty)
        list(SORT _dirty)
    endif()

    set(_overlay "")
    foreach(_p IN LISTS _dirty)
        if(EXISTS "${SIMDIGEST_WORKING_DIR}/${_p}")
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" hash-object -- "${_p}"
                WORKING_DIRECTORY "${SIMDIGEST_WORKING_DIR}"
                RESULT_VARIABLE _hx
                OUTPUT_VARIABLE _sha
                ERROR_VARIABLE _herr
                OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(NOT _hx EQUAL 0)
                message(FATAL_ERROR "SimID: 'git hash-object -- ${_p}' failed (exit ${_hx}): ${_herr}")
            endif()
        else()
            set(_sha "DELETED")
        endif()
        # Only record a real difference. This discards the two pseudo-modifications
        # that differ by platform rather than by content: a core.fileMode exec-bit
        # flip (true on macOS, false on Windows) and a stale-index eol renormalisation.
        string(FIND "${_tree}" " ${_sha}\t${_p}" _found)
        if(_found EQUAL -1)
            string(APPEND _overlay "${_p}\t${_sha}\n")
        endif()
    endforeach()

    string(REPLACE ";" "\n" _tree_text "${_tree_lines}")
    set(${_out_text} "${_tree_text}\n#dirty\n${_overlay}" PARENT_SCOPE)
endfunction()

function(ComputeSimSourceDigest _out)
    set(_code_paths ${ARGN})

    if(SIMDIGEST_ALLOW_NO_GIT OR NOT GIT_EXECUTABLE OR NOT EXISTS "${SIMDIGEST_WORKING_DIR}/.git")
        set(${_out} "00000000" PARENT_SCOPE)
        return()
    endif()

    SimDigestCollect(_code "${SIMDIGEST_CODE_EXT_REGEX}" ${_code_paths})
    SimDigestCollect(_build "${SIMDIGEST_BUILD_EXT_REGEX}" ${SIMDIGEST_PATHS_BUILD})

    string(SHA256 _h "SIMID/2\nCODE\n${_code}\nBUILD\n${_build}")
    string(SUBSTRING "${_h}" 0 8 _short)
    string(TOUPPER "${_short}" _short)
    set(${_out} "${_short}" PARENT_SCOPE)
endfunction()

# Function: SetupSimDigestMonitoring
# Description: registers the build-time re-invocation of this script. Everything the
#              script-mode process needs is forwarded with -D; see the header note.
function(SetupSimDigestMonitoring)
    add_custom_target(check_sim_source_digest
        ALL
        DEPENDS ${SIMDIGEST_PRE_CONFIGURE_FILE}
        BYPRODUCTS ${SIMDIGEST_POST_CONFIGURE_FILE}
        COMMENT "Computing the SimID source digest..."
        COMMAND
            ${CMAKE_COMMAND}
            -DSIMDIGEST_BUILD_TIME=TRUE
            -DSIMDIGEST_WORKING_DIR=${SIMDIGEST_WORKING_DIR}
            -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
            -DSIMDIGEST_ALLOW_NO_GIT=${SIMDIGEST_ALLOW_NO_GIT}
            -DSIMDIGEST_PRE_CONFIGURE_FILE=${SIMDIGEST_PRE_CONFIGURE_FILE}
            -DSIMDIGEST_POST_CONFIGURE_FILE=${SIMDIGEST_POST_CONFIGURE_FILE}
            -P "${CMAKE_CURRENT_LIST_FILE}")
endfunction()

# Function: Main
# Description: primary entry-point. Selects configure-time or build-time behaviour.
function(Main)
    if(SIMDIGEST_BUILD_TIME)
        ComputeSimSourceDigest(SIM_SOURCE_DIGEST_ZEROHOUR
            ${SIMDIGEST_PATHS_CODE_SHARED} ${SIMDIGEST_PATHS_CODE_ZH})
        ComputeSimSourceDigest(SIM_SOURCE_DIGEST_GENERALS
            ${SIMDIGEST_PATHS_CODE_SHARED} ${SIMDIGEST_PATHS_CODE_G})
        message(STATUS "SimID source digest: ZH=0x${SIM_SOURCE_DIGEST_ZEROHOUR} G=0x${SIM_SOURCE_DIGEST_GENERALS}")
        configure_file("${SIMDIGEST_PRE_CONFIGURE_FILE}" "${SIMDIGEST_POST_CONFIGURE_FILE}" @ONLY)
    else()
        SetupSimDigestMonitoring()
    endif()
endfunction()

# And off we go...
Main()
