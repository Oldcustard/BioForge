#include "pch.h"

#include "Config.h"
#include "SkyrimNetAPI.h"
#include "StagingStore.h"
#include "UI.h"

namespace
{
    // SkyrimNet Beta 25 = Public API v10, and Beta 25 is what BioForge now
    // needs: the content library (bios across layers, commits through the
    // dashboard's HTTP API) and the prompt templates shipped as an external
    // layer simply do not exist in a Beta 24 install. v8 gave us
    // PublicSendCustomPromptToLLM, but against anything older than v10 the
    // prompts would not resolve and commits would go nowhere - so warn loudly
    // and stay read-only rather than half-work.
    constexpr int kMinUsefulAPIVersion = 10;

    // SkyrimNet's own header specifies kDataLoaded for FindFunctions(): registration
    // works from there, and data queries stay safe (they return empty) until a save
    // loads. The SMF page is registered earlier, in SKSEPlugin_Load, and only reads
    // any of this when the user actually opens it.
    void OnMessage(SKSE::MessagingInterface::Message* a_msg)
    {
        if (!a_msg || a_msg->type != SKSE::MessagingInterface::kDataLoaded) {
            return;
        }

        if (!BioForge::SN::Init()) {
            logs::error("SkyrimNet.dll not found - BioForge needs SkyrimNet installed"sv);
            return;
        }

        const auto version = BioForge::SN::Version();
        if (version < kMinUsefulAPIVersion) {
            logs::warn("SkyrimNet API v{} is older than v{}; generation will not be available"sv,
                       version, kMinUsefulAPIVersion);
        } else {
            logs::info("SkyrimNet API v{}"sv, version);
        }

        BioForge::Config::Load();

        // Staging is per-session scratch. Anything left uncommitted last time is
        // not in use by anything, so it goes rather than accumulating.
        BioForge::Staging::ClearStaged();
    }
}

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    logs::info("BioForge loaded"sv);

    const auto messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnMessage)) {
        SKSE::stl::report_and_fail("BioForge: failed to register the SKSE message listener"sv);
    }

    // SMF ships a preload marker, so its DLL is already in the process by the time
    // ordinary SKSE plugins load - registering here matches SMF's own example.
    BioForge::UI::Register();

    return true;
}
