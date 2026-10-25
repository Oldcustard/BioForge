# SKSEPlugin.cmake - Setup for SKSE plugin using CommonLibSSE-NG submodule


# CommonLibSSE-NG (alandtse/CommonLibVR branch ng).
# Normally the submodule at dll-source/lib/commonlibsse-ng. BIOFORGE_COMMONLIB_DIR
# lets a local build point at an existing 500MB checkout instead of cloning again.
set(BIOFORGE_COMMONLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dll-source/lib/commonlibsse-ng" CACHE PATH "Path to a CommonLibSSE-NG source tree")
if(NOT EXISTS "${BIOFORGE_COMMONLIB_DIR}/cmake/CommonLibSSE.cmake")
    message(FATAL_ERROR
        "CommonLibSSE-NG not found at ${BIOFORGE_COMMONLIB_DIR}. "
        "Init the submodule, or configure with -DBIOFORGE_COMMONLIB_DIR=<path>.")
endif()
# CommonLibSSE-NG configuration
set(CommonLibPath "${BIOFORGE_COMMONLIB_DIR}")

# Save original values
set(_saved_build_type "${CMAKE_BUILD_TYPE}")
set(_saved_cmake_message_log_level "${CMAKE_MESSAGE_LOG_LEVEL}")
set(_saved_fetchcontent_base_dir "${FETCHCONTENT_BASE_DIR}")

set(CMAKE_MESSAGE_LOG_LEVEL WARNING)

# Always build CommonLibSSE in Release mode to disable assertions and enable optimizations
set(CMAKE_BUILD_TYPE "Release")
add_definitions(-D_CRT_SECURE_NO_WARNINGS)
add_definitions(-DNDEBUG)

# Disable CommonLibSSE tests when building as subdirectory
set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLibSSE tests" FORCE)

# Add CommonLibSSE-NG as a subdirectory with EXCLUDE_FROM_ALL. 
get_filename_component(BIOFORGE_BUILD_ROOT "${CMAKE_BINARY_DIR}/.." ABSOLUTE)
# FetchContent (CommonLibSSE-NG's hde64 download) defaults to <binary dir>/_deps.
# Point it at the same shared folder. 
set(FETCHCONTENT_BASE_DIR "${BIOFORGE_BUILD_ROOT}/external_builds/_deps")
add_subdirectory("${BIOFORGE_COMMONLIB_DIR}" "${BIOFORGE_BUILD_ROOT}/external_builds/commonlibsse-ng")

# Restore FETCHCONTENT_BASE_DIR.
if(NOT "${_saved_fetchcontent_base_dir}" STREQUAL "")
    set(FETCHCONTENT_BASE_DIR "${_saved_fetchcontent_base_dir}")
else()
    unset(FETCHCONTENT_BASE_DIR)
endif()

# Include the CommonLibSSE helper cmake functions (provides add_commonlibsse_plugin macro)
include("${BIOFORGE_COMMONLIB_DIR}/cmake/CommonLibSSE.cmake")

# Restore original build type for the main project
set(CMAKE_BUILD_TYPE "${_saved_build_type}" CACHE STRING "" FORCE)
if(DEFINED _saved_cmake_message_log_level AND NOT "${_saved_cmake_message_log_level}" STREQUAL "")
    set(CMAKE_MESSAGE_LOG_LEVEL "${_saved_cmake_message_log_level}")
    set(CMAKE_MESSAGE_LOG_LEVEL "${_saved_cmake_message_log_level}" CACHE STRING "" FORCE)
else()
    unset(CMAKE_MESSAGE_LOG_LEVEL)
endif()
