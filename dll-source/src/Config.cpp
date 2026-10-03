#include "pch.h"

#include "Config.h"
#include "SkyrimNetAPI.h"

namespace BioForge::Config
{
    namespace
    {
        Settings g_settings{};

        // Re-reading is ~9 round trips through SkyrimNet's config store. That
        // is nothing once a second and silly once a frame.
        constexpr auto kRefreshInterval = std::chrono::milliseconds{ 1000 };

        std::chrono::steady_clock::time_point g_lastRead{};

        // SkyrimNet hands config values back as strings. Anything we cannot parse
        // keeps the compiled-in default rather than silently becoming zero.
        float ReadFloat(const char* a_path, float a_fallback)
        {
            const auto raw = SN::PluginConfigValue("BioForge", a_path, "");
            if (raw.empty()) {
                return a_fallback;
            }
            try {
                return std::stof(raw);
            } catch (...) {
                return a_fallback;
            }
        }

        int ReadInt(const char* a_path, int a_fallback)
        {
            const auto raw = SN::PluginConfigValue("BioForge", a_path, "");
            if (raw.empty()) {
                return a_fallback;
            }
            try {
                return std::stoi(raw);
            } catch (...) {
                return a_fallback;
            }
        }

        bool ReadBool(const char* a_path, bool a_fallback)
        {
            auto raw = SN::PluginConfigValue("BioForge", a_path, "");
            if (raw.empty()) {
                return a_fallback;
            }
            std::transform(raw.begin(), raw.end(), raw.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (raw == "true" || raw == "1" || raw == "yes" || raw == "on") {
                return true;
            }
            if (raw == "false" || raw == "0" || raw == "no" || raw == "off") {
                return false;
            }
            return a_fallback;
        }
    }

    namespace
    {
        Settings ReadAll()
        {
            const Settings defaults{};

            Settings s{};
            s.exteriorScanRadius = ReadFloat("scan.exteriorRadius", defaults.exteriorScanRadius);
            s.uniqueOnly  = ReadBool("scan.uniqueOnly", defaults.uniqueOnly);
            s.includeDead = ReadBool("scan.includeDead", defaults.includeDead);

            s.maxConcurrent =
                std::clamp(ReadInt("generate.maxConcurrent", defaults.maxConcurrent), 1, 8);

            s.refinePass = ReadBool("generate.refinePass", defaults.refinePass);
            s.ownVariant = ReadBool("llm.useOwnVariant", defaults.ownVariant);

            s.digestEnabled   = ReadBool("digest.enabled", defaults.digestEnabled);
            s.digestAutoBuild = ReadBool("digest.autoBuild", defaults.digestAutoBuild);
            s.digestMaxCandidates = std::clamp(
                ReadInt("digest.maxCandidates", defaults.digestMaxCandidates), 20, 400);
            return s;
        }

        void LogSettings(const char* a_what)
        {
            logs::info("{}: exteriorRadius={:.0f} uniqueOnly={} includeDead={}"
                       " maxConcurrent={} refinePass={}"sv,
                       a_what, g_settings.exteriorScanRadius,
                       g_settings.uniqueOnly, g_settings.includeDead,
                       g_settings.maxConcurrent, g_settings.refinePass);
            logs::info("{}: digest enabled={} autoBuild={} maxCandidates={}"sv,
                       a_what, g_settings.digestEnabled, g_settings.digestAutoBuild,
                       g_settings.digestMaxCandidates);
            logs::info("{}: llm variant={}"sv, a_what, LlmVariant());
        }
    }

    void Load()
    {
        g_settings = ReadAll();
        g_lastRead = std::chrono::steady_clock::now();
        LogSettings("config");
    }

    void Refresh()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - g_lastRead < kRefreshInterval) {
            return;
        }
        g_lastRead = now;

        auto next = ReadAll();
        if (next == g_settings) {
            return;   // the common case: say nothing
        }
        g_settings = next;

        // Worth a line. A setting changing under a running session is exactly
        // the sort of thing you want to find in the log afterwards.
        LogSettings("config changed");
    }

    const Settings& Get() { return g_settings; }

    const char* LlmVariant()
    {
        // SkyrimNet's own profile-writing variant by default: BioForge does
        // the job it is already configured for, so the player picks a model
        // once and inherits its tuning (full model, 10k max_tokens, temp 0.7).
        // `bioforge` is for players who want bios on a different model than
        // SkyrimNet's own profile generation (issue #4).
        return g_settings.ownVariant ? "bioforge" : "CharacterProfileGeneration";
    }
}
