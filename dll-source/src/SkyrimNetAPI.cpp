#include "pch.h"

// The one and only inclusion of the vendor header - see SkyrimNetAPI.h.
#include "SkyrimNet_PublicAPI.h"

#include "SkyrimNetAPI.h"

namespace BioForge::SN
{
    namespace
    {
        bool g_ready   = false;
        int  g_version = 0;
    }

    bool Init()
    {
        g_ready   = FindFunctions();
        g_version = (g_ready && PublicGetVersion) ? PublicGetVersion() : 0;
        return g_ready;
    }

    bool Available() { return g_ready; }
    int  Version()   { return g_version; }

    std::string BioTemplateName(std::uint32_t a_refFormID)
    {
        if (!g_ready || !PublicGetBioTemplateName) {
            return {};
        }
        return PublicGetBioTemplateName(a_refFormID);
    }

    bool MemorySystemReady()
    {
        return g_ready && PublicIsMemorySystemReady && PublicIsMemorySystemReady();
    }

    std::string PluginConfigValue(const char* a_plugin, const char* a_path, const char* a_fallback)
    {
        if (!g_ready || !PublicGetPluginConfigValue) {
            return a_fallback ? a_fallback : "";
        }
        return PublicGetPluginConfigValue(a_plugin, a_path, a_fallback);
    }
}
