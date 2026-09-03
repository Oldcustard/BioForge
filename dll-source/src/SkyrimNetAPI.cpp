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

    bool CanGenerate()
    {
        return g_ready && PublicSendCustomPromptToLLM != nullptr;
    }

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

    std::uint64_t FormIDToUUID(std::uint32_t a_formID)
    {
        if (!g_ready || !PublicFormIDToUUID) {
            return 0;
        }
        return PublicFormIDToUUID(a_formID);
    }

    std::string RelatedActors(std::uint32_t a_refFormID, int a_maxCount)
    {
        if (!g_ready || !PublicGetRelatedActors) {
            return {};
        }
        // Windows sized for "who has this person actually been around": the
        // last ten minutes and the last hour.
        return PublicGetRelatedActors(a_refFormID, a_maxCount, 600.0, 3600.0);
    }

    std::string WorldKnowledgeForActor(std::uint32_t a_refFormID, int a_maxResults)
    {
        if (!g_ready || g_version < 9 || !PublicGetWorldKnowledgeForActor) {
            return {};
        }
        // Empty query = always-inject entries only, which is the characterful
        // subset; semantic matching against a bio prompt would just echo the
        // dialogue harvest we already collect.
        return PublicGetWorldKnowledgeForActor(a_refFormID, a_maxResults, "");
    }

    bool SendCustomPrompt(const char* a_promptName, const char* a_variant,
                          const char* a_contextJson, LLMPromptCallback a_callback)
    {
        if (!CanGenerate()) {
            return false;
        }
        return PublicSendCustomPromptToLLM(a_promptName, a_variant, a_contextJson,
                                           std::move(a_callback));
    }

    std::string PluginConfigValue(const char* a_plugin, const char* a_path, const char* a_fallback)
    {
        if (!g_ready || !PublicGetPluginConfigValue) {
            return a_fallback ? a_fallback : "";
        }
        return PublicGetPluginConfigValue(a_plugin, a_path, a_fallback);
    }
}
