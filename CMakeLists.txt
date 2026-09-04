cmake_minimum_required(VERSION 3.21)

# Static libs, DYNAMIC CRT. Both halves matter:
#  - static libs keep fmt.dll / spdlog.dll out of SKSE/Plugins, where a loose
#    dependency DLL can collide with another plugin's copy;
#  - /MD is REQUIRED by SkyrimNet's public API, which passes std::string and
#    std::function across the DLL boundary and so needs a shared CRT heap.
# Must be set before project() to reach the vcpkg toolchain.
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "vcpkg triplet")
endif()
# Bump on every release: this becomes the SKSEPluginInfo version and the first
# line of BioForge.log, which is how a bug report gets matched to a build.
project(BioForge VERSION 0.1.0 LANGUAGES CXX)

# CommonLibSSE-NG (alandtse/CommonLibVR branch ng).
# Normally the submodule at dll-source/lib/commonlibsse-ng. BIOFORGE_COMMONLIB_DIR
# lets a local build point at an existing 500MB checkout instead of cloning again.
set(BIOFORGE_COMMONLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dll-source/lib/commonlibsse-ng"
    CACHE PATH "Path to a CommonLibSSE-NG source tree")
if(NOT EXISTS "${BIOFORGE_COMMONLIB_DIR}/cmake/CommonLibSSE.cmake")
    message(FATAL_ERROR
        "CommonLibSSE-NG not found at ${BIOFORGE_COMMONLIB_DIR}. "
        "Init the submodule, or configure with -DBIOFORGE_COMMONLIB_DIR=<path>.")
endif()

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${BIOFORGE_COMMONLIB_DIR}" commonlib)

# A plain source add_subdirectory does NOT define add_commonlibsse_plugin.
if(NOT COMMAND add_commonlibsse_plugin)
    include("${BIOFORGE_COMMONLIB_DIR}/cmake/CommonLibSSE.cmake")
endif()

add_commonlibsse_plugin(${PROJECT_NAME}
    AUTHOR "Bio Forge"
    SOURCES
        dll-source/src/main.cpp
        dll-source/src/SkyrimNetAPI.cpp
        dll-source/src/Config.cpp
        dll-source/src/ScopeSelector.cpp
        dll-source/src/Generator.cpp
        dll-source/src/Placement.cpp
        dll-source/src/RegionDigest.cpp
        dll-source/src/StagingStore.cpp
        dll-source/src/UI.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE dll-source/include dll-source/src)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_options(${PROJECT_NAME} PRIVATE /Zc:preprocessor)   # CommonLib headers require it
target_precompile_headers(${PROJECT_NAME} PRIVATE dll-source/src/pch.h)

# Drop the built DLL (+pdb) into a Data-shaped mod folder so it can be zipped
# or pointed at directly by MO2. Visual Studio is a multi-config generator and
# appends the config name to RUNTIME_OUTPUT_DIRECTORY, which would bury the DLL
# in a Release/ subfolder - so set the per-config properties too.
set(BIOFORGE_STAGE "${CMAKE_BINARY_DIR}/BioForge/SKSE/Plugins")
set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BIOFORGE_STAGE}")
foreach(cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${BIOFORGE_STAGE}")
endforeach()

# Ship the SkyrimNet plugin manifest alongside the DLL so the staged folder is a
# complete, installable mod.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/mod-root" "${CMAKE_BINARY_DIR}/BioForge")
