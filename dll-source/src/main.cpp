#include "pch.h"

#include "Config.h"
#include "ScopeSelector.h"
#include "SkyrimNetAPI.h"

namespace
{
    // SkyrimNet's PublicSendCustomPromptToLLM arrived in API v8. Everything phase 1
    // touches is v3+, but there is no point loading against a SkyrimNet we cannot
    // eventually generate with, so warn loudly and keep going read-only.
    constexpr int kMinUsefulAPIVersion = 8;

    void RunGapScan()
    {
        if (!BioForge::SN::Available()) {
            logs::warn("scan requested but SkyrimNet is not available"sv);
            return;
        }
        BioForge::LogGapReport(BioForge::Scan());
    }

    class HotkeySink : public RE::BSTEventSink<RE::InputEvent*>
    {
    public:
        static HotkeySink* GetSingleton()
        {
            static HotkeySink singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const*        a_event,
                                              RE::BSTEventSource<RE::InputEvent*>*) override
        {
            const auto hotkey = BioForge::Config::Get().scanHotkey;
            if (!a_event || hotkey == 0) {
                return RE::BSEventNotifyControl::kContinue;
            }

            for (auto* e = *a_event; e; e = e->next) {
                if (e->eventType != RE::INPUT_EVENT_TYPE::kButton) {
                    continue;
                }
                const auto* button = e->AsButtonEvent();
                if (!button || !button->IsDown()) {
                    continue;   // IsDown() is the press edge; IsPressed() would repeat
                }
                if (button->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
                    continue;
                }
                if (button->GetIDCode() == hotkey) {
                    RunGapScan();
                }
            }

            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        HotkeySink()                             = default;
        HotkeySink(const HotkeySink&)            = delete;
        HotkeySink& operator=(const HotkeySink&) = delete;
    };

    // SkyrimNet's own header specifies kDataLoaded for FindFunctions(): action and
    // decorator registration works from there, and data queries stay safe (they
    // return empty) until a save loads.
    void OnMessage(SKSE::MessagingInterface::Message* a_msg)
    {
        if (!a_msg || a_msg->type != SKSE::MessagingInterface::kDataLoaded) {
            return;
        }

        if (!BioForge::SN::Init()) {
            logs::error("SkyrimNet.dll not found - Bio Forge needs SkyrimNet installed"sv);
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

        if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
            input->AddEventSink<RE::InputEvent*>(HotkeySink::GetSingleton());
            logs::info("scan hotkey bound to scan code 0x{:02X}"sv,
                       BioForge::Config::Get().scanHotkey);
        } else {
            logs::error("no input device manager - scan hotkey unavailable"sv);
        }
    }
}

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    logs::info("Bio Forge loaded"sv);

    const auto messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnMessage)) {
        SKSE::stl::report_and_fail("Bio Forge: failed to register the SKSE message listener"sv);
    }

    return true;
}
