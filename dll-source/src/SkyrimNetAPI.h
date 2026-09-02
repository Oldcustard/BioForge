#pragma once

#include <cstdint>
#include <string>

// Thin wrapper over SkyrimNet's exported C API.
//
// SkyrimNet_PublicAPI.h DEFINES its function pointers at namespace scope, so
// including it in more than one translation unit is a link error. It is included
// by SkyrimNetAPI.cpp and nowhere else; everything else goes through here.
namespace BioForge::SN
{
    // Resolve SkyrimNet's exports. Call once from kDataLoaded. Safe to call when
    // SkyrimNet is absent - returns false and every accessor then degrades.
    bool Init();

    bool Available();
    int  Version();

    // The canonical bio template name for a placed reference, or "" if SkyrimNet
    // has none. Always prefer this over recomputing the "<name>_<3 hex>" key:
    // the key is built from the REFERENCE FormID, not the base record.
    std::string BioTemplateName(std::uint32_t refFormID);

    // Data queries return empty until a save is loaded.
    bool MemorySystemReady();

    std::string PluginConfigValue(const char* pluginName, const char* path, const char* fallback);
}
