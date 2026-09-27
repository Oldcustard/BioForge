#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Thin wrapper over SkyrimNet's exported C API.
//
// SkyrimNet_PublicAPI.h DEFINES its function pointers at namespace scope, so
// including it in more than one translation unit is a link error. It is included
// by SkyrimNetAPI.cpp and nowhere else; everything else goes through here.
namespace BioForge::SN
{
    // SkyrimNet invokes this on one of its ThreadPool workers when the LLM
    // answers. `response` is only valid for the duration of the call.
    using LLMPromptCallback = std::function<void(const char* a_response, int a_success)>;

    // Resolve SkyrimNet's exports. Call once from kDataLoaded. Safe to call when
    // SkyrimNet is absent - returns false and every accessor then degrades.
    bool Init();

    bool Available();
    int  Version();

    // v10+ (SkyrimNet Beta 25): the whole pipeline assumes the content
    // library - prompt templates resolve from layers, bios live across
    // layers, commits go through the dashboard API. False against anything
    // older, in which case BioForge can scan but never generate.
    bool CanGenerate();

    // The canonical bio template name for a placed reference, or "" if SkyrimNet
    // has none. Always prefer this over recomputing the "<name>_<3 hex>" key:
    // the key is built from the REFERENCE FormID, not the base record.
    std::string BioTemplateName(std::uint32_t refFormID);

    // Data queries return empty until a save is loaded.
    bool MemorySystemReady();

    // The loaded playthrough's id, or "" before a save loads. It is also the
    // folder name of that playthrough's per-save layer, saves/<id>/.
    std::string SaveUniqueID();

    // SkyrimNet's internal id for a placed actor, or 0 when unknown. The prompt
    // context wants it as a HEX STRING - decorators misresolve decimal strings
    // and reject JSON numbers outright.
    std::uint64_t FormIDToUUID(std::uint32_t a_formID);

    // JSON array of actors recently near the target, or "" before a save loads.
    std::string RelatedActors(std::uint32_t a_refFormID, int a_maxCount);

    // JSON array of world-knowledge entries for the target (v9+; "" on older
    // SkyrimNet and when nothing is always-inject relevant).
    std::string WorldKnowledgeForActor(std::uint32_t a_refFormID, int a_maxResults);

    // Render the named prompt template against `a_contextJson` (merged into the
    // render context as top-level variables) and send it to the user's
    // configured LLM. Returns false only when the task could not be queued.
    bool SendCustomPrompt(const char* a_promptName, const char* a_variant,
                          const char* a_contextJson, LLMPromptCallback a_callback);

    std::string PluginConfigValue(const char* pluginName, const char* path, const char* fallback);
}
