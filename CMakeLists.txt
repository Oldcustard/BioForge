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
project(BioForge VERSION 1.2.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/dll-source/cmake")
include(commonlib)

find_package(directxtk CONFIG REQUIRED) # needed by commonlib

add_commonlibsse_plugin(${PROJECT_NAME}
    VERSION ${PROJECT_VERSION}
    AUTHOR "BioForge"
    USE_ADDRESS_LIBRARY
    MINIMUM_SKSE_VERSION "2.0.1.04"
    SOURCES
    dll-source/src/main.cpp
    dll-source/src/SkyrimNetAPI.cpp
    dll-source/src/SkyrimNetWeb.cpp
    dll-source/src/Config.cpp
    dll-source/src/ContentLibrary.cpp
    dll-source/src/ScopeSelector.cpp
    dll-source/src/Generator.cpp
    dll-source/src/Placement.cpp
    dll-source/src/RegionDigest.cpp
    dll-source/src/StagingStore.cpp
    dll-source/src/SeverBio.cpp
    dll-source/src/BlockRules.cpp
    dll-source/src/UI.cpp)

# Commits ride SkyrimNet's loopback web server through WinHTTP.
target_link_libraries(${PROJECT_NAME} PRIVATE
    Microsoft::DirectXTK # needed by commonlib
    CommonLibSSE::CommonLibSSE
    winhttp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    dll-source/include
    dll-source/src
)

# One version, from project() above: the web client's user agent reads it
# rather than carrying its own copy. The content layer's manifest.json and
# settings/BioForge.yaml are stamped from it in POST_BUILD below.
target_compile_definitions(${PROJECT_NAME} PRIVATE BIOFORGE_VERSION="${PROJECT_VERSION}")

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE dll-source/src/pch.h)

# Release links without /DEBUG, so MSVC writes no BioForge.pdb and the POST_BUILD
# copy fails. Ask for a full PDB in the optimised configs so it can be staged.
# /DEBUG turns off the default /OPT:REF and /OPT:ICF, so restore them explicitly
# to keep dead-code removal and function folding.
target_link_options(${PROJECT_NAME} PRIVATE
    "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/DEBUG:FULL;/OPT:REF;/OPT:ICF>")

# The mod folder is assembled in POST_BUILD from the DLL, PDB and mod-root, so
# it can be zipped or pointed at directly by MO2. The linker keeps CMake's
# default output location, which avoids the Visual Studio Release/ subfolder.
set(BIOFORGE_STAGE "${CMAKE_BINARY_DIR}/BioForge")
set(BIOFORGE_STAGE_PLUGINS "${BIOFORGE_STAGE}/SKSE/Plugins")

# Stamp the content layer's version fields from PROJECT_VERSION, then ship the
# SkyrimNet plugin manifest alongside the DLL so the staged folder is a
# complete, installable mod. Stamping runs first so the staged copy is current.
set(BIOFORGE_LAYER "${CMAKE_CURRENT_SOURCE_DIR}/mod-root/SKSE/Plugins/SkyrimNet/external/oldcustard.bioforge")
set(BIOFORGE_STAMP_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/dll-source/cmake/stamp_version.cmake")

# Licenses ship in BioForge/LICENSES: the project's own license, the notices
# report, the vendored headers' licenses, CommonLibSSE-NG (GPL-3.0) with its
# bundled hde64 license, and the copyright file of each vcpkg package. The
# vcpkg copies come from the installed tree, so they match the built versions.
set(BIOFORGE_LICENSES "${BIOFORGE_STAGE}/LICENSES/${PROJECT_NAME}")
set(BIOFORGE_VCPKG_SHARE "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")

# Rebuild the staging folder from scratch on every build, so files removed from
# mod-root or from the license list do not linger.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${BIOFORGE_STAGE}"
    COMMAND ${CMAKE_COMMAND} "-DINPUT=${BIOFORGE_LAYER}/manifest.json" "-DVERSION=${PROJECT_VERSION}" "-DKEY=version" -P "${BIOFORGE_STAMP_SCRIPT}"
    COMMAND ${CMAKE_COMMAND} "-DINPUT=${BIOFORGE_LAYER}/settings/BioForge.yaml" "-DVERSION=${PROJECT_VERSION}" "-DKEY=version" -P "${BIOFORGE_STAMP_SCRIPT}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/mod-root" "${BIOFORGE_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BIOFORGE_STAGE_PLUGINS}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${PROJECT_NAME}>" "${BIOFORGE_STAGE_PLUGINS}"
    # PDB file copy - comment out to skip copying the debug symbols
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${BIOFORGE_STAGE_PLUGINS}"

    # License Files - update if any licenses change or new dependencies are added
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BIOFORGE_LICENSES}/vcpkg"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${BIOFORGE_LICENSES}/BioForge-GPL-3.0.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/dll-source/licenses/THIRD-PARTY-NOTICES.txt" "${BIOFORGE_LICENSES}/THIRD-PARTY-NOTICES.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/dll-source/include/SkyrimNet/CppAPI/LICENSE" "${BIOFORGE_LICENSES}/SkyrimNet-CppAPI-MIT.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/dll-source/include/SKSEMenuFramework/LICENSE" "${BIOFORGE_LICENSES}/SKSEMenuFramework-LGPL-2.1.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${BIOFORGE_COMMONLIB_DIR}/COPYING.txt" "${BIOFORGE_LICENSES}/CommonLibSSE-NG-GPL-3.0.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${BIOFORGE_COMMONLIB_DIR}/EXCEPTIONS.md" "${BIOFORGE_LICENSES}/CommonLibSSE-NG-EXCEPTIONS.md"
    COMMAND ${CMAKE_COMMAND} -E copy "${BIOFORGE_COMMONLIB_DIR}/licenses/LICENSE-hde64.txt" "${BIOFORGE_LICENSES}/hde64-BSD-2-Clause.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${BIOFORGE_VCPKG_SHARE}/directxtk/copyright" "${BIOFORGE_LICENSES}/vcpkg/directxtk-copyright.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${BIOFORGE_VCPKG_SHARE}/directxmath/copyright" "${BIOFORGE_LICENSES}/vcpkg/directxmath-copyright.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${BIOFORGE_VCPKG_SHARE}/fmt/copyright" "${BIOFORGE_LICENSES}/vcpkg/fmt-copyright.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${BIOFORGE_VCPKG_SHARE}/spdlog/copyright" "${BIOFORGE_LICENSES}/vcpkg/spdlog-copyright.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${BIOFORGE_VCPKG_SHARE}/rapidcsv/copyright" "${BIOFORGE_LICENSES}/vcpkg/rapidcsv-copyright.txt"
)
