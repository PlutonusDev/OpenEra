cmake_minimum_required(VERSION 3.25)

project(OpenEra
    VERSION 0.1.2
    DESCRIPTION "Compatibility layer for running Xbox One and Xbox Series X|S titles on desktop Windows"
    LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "OpenEra only supports 64-bit (x64) builds.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OPENERA_BUILD_TESTS "Build the OpenEra unit tests" ON)
# What openera --version reports. Release builds pass the released version.
set(OPENERA_VERSION "${PROJECT_VERSION}-dev" CACHE STRING "Version string openera reports")
option(OPENERA_ENABLE_CORE_CHECK "Run C++ Core Check (/analyze) on project sources (MSVC only)" ON)

# ---------------------------------------------------------------------------
# Shared compiler settings for OpenEra's own targets.
# Applied per target so third-party code (Catch2) keeps its own flags.
# ---------------------------------------------------------------------------
add_library(OpenEraCompileOptions INTERFACE)
add_library(OpenEra::CompileOptions ALIAS OpenEraCompileOptions)

if(MSVC)
    # Covers both MSVC (cl) and Clang-CL.
    target_compile_options(OpenEraCompileOptions INTERFACE
        /W4 /WX
        /permissive-
        /utf-8
        /Zc:__cplusplus)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # MSVC only: Clang-CL already has a conforming preprocessor and reports this flag as unused.
        target_compile_options(OpenEraCompileOptions INTERFACE /Zc:preprocessor)
    endif()
    target_compile_definitions(OpenEraCompileOptions INTERFACE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE)
else()
    message(FATAL_ERROR "OpenEra must be built with MSVC or Clang-CL (x86_64-pc-windows-msvc).")
endif()

# C++ Core Check runs only under MSVC; Clang-CL does not support /analyze plugins.
add_library(OpenEraCoreCheck INTERFACE)
add_library(OpenEra::CoreCheck ALIAS OpenEraCoreCheck)

if(OPENERA_ENABLE_CORE_CHECK AND MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # The rule set includes its sibling .ruleset files by relative path, so the
    # rule set directory must be passed as well.
    # Forward slashes: SHELL: option parsing would strip backslashes.
    cmake_path(SET _openeraRuleSetDir NORMALIZE "$ENV{VSINSTALLDIR}/Team Tools/Static Analysis Tools/Rule Sets")
    if(DEFINED ENV{VSINSTALLDIR} AND EXISTS "${_openeraRuleSetDir}/CppCoreCheckRules.ruleset")
        message(STATUS "C++ Core Check: enabled")
        target_compile_options(OpenEraCoreCheck INTERFACE
            # Treat <...> includes (STL, Windows SDK) as external and skip them.
            /external:anglebrackets
            /external:W0
            /analyze
            /analyze:external-
            "SHELL:/analyze:plugin EspXEngine.dll"
            "SHELL:/analyze:rulesetdirectory \"${_openeraRuleSetDir}\""
            "SHELL:/analyze:ruleset CppCoreCheckRules.ruleset")
    else()
        message(WARNING "C++ Core Check rule set not found; run CMake from a Visual Studio Developer shell to enable it.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Sources and tests
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(OPENERA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
