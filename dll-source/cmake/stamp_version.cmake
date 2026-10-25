# stamp_version.cmake
#
# Rewrite one version field in a file, in place.
#
# Driven by the POST_BUILD step in the top-level CMakeLists.txt. The content
# layer carries a version of its own in two places - the layer's manifest.json
# and the plugin block of settings/BioForge.yaml - and both are stamped from
# PROJECT_VERSION so the mod, its settings and the DLL can never disagree
# about which build they are.
#
# Usage:
#   cmake -DINPUT=<file> -DVERSION=<x.y.z> -DKEY=<field name> -P stamp_version.cmake
#
# KEY names the field to stamp. The field must sit at the start of a line
# (optionally indented, optionally quoted) and hold a quoted string, which
# covers both the JSON "version": "1.1.1" and the YAML version: "1.1.1".
# Anchoring to the line start is what keeps "min_skyrimnet_version" from
# being mistaken for "version".

if(NOT DEFINED INPUT OR NOT DEFINED VERSION OR NOT DEFINED KEY)
    message(FATAL_ERROR "stamp_version.cmake: INPUT, VERSION and KEY are all required")
endif()

if(NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "stamp_version.cmake: no such file: ${INPUT}")
endif()

file(READ "${INPUT}" content)

# (^|\n) anchors the field to the start of a line, so a field that merely
# ends in "version" is never touched.
string(REGEX MATCH "(^|\n)[ \t]*\"?${KEY}\"?[ \t]*:[ \t]*\"([^\"]*)\"" found "${content}")
if(NOT found)
    message(FATAL_ERROR "stamp_version.cmake: no '${KEY}' field in ${INPUT}")
endif()
set(current "${CMAKE_MATCH_2}")

if("${current}" STREQUAL "${VERSION}")
    message(STATUS "stamp_version: ${INPUT} already at ${VERSION}")
    return()
endif()

# \1 keeps the line start, \2 keeps the key and its quoting exactly as they
# were, so the file's own style survives the rewrite.
string(REGEX REPLACE "(^|\n)([ \t]*\"?${KEY}\"?[ \t]*:[ \t]*)\"[^\"]*\"" "\\1\\2\"${VERSION}\"" updated "${content}")

file(WRITE "${INPUT}" "${updated}")
message(STATUS "stamp_version: ${INPUT} ${current} -> ${VERSION}")
