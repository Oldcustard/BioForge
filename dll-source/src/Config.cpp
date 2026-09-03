#include "pch.h"

#include "Config.h"
#include "SkyrimNetAPI.h"

namespace BioForge::Config
{
    namespace
    {
        Settings g_settings{};

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

    void Load()
    {
        const Settings defaults{};
        g_settings.scanRadius  = ReadFloat("scan.radius", defaults.scanRadius);
        g_settings.cellOnly    = ReadBool("scan.cellOnly", defaults.cellOnly);
        g_settings.uniqueOnly  = ReadBool("scan.uniqueOnly", defaults.uniqueOnly);
        g_settings.includeDead = ReadBool("scan.includeDead", defaults.includeDead);

        g_settings.maxConcurrent =
            std::clamp(ReadInt("generate.maxConcurrent", defaults.maxConcurrent), 1, 8);

        g_settings.refinePass = ReadBool("generate.refinePass", defaults.refinePass);

        g_settings.digestEnabled   = ReadBool("digest.enabled", defaults.digestEnabled);
        g_settings.digestAutoBuild = ReadBool("digest.autoBuild", defaults.digestAutoBuild);
        g_settings.digestMaxCandidates = std::clamp(
            ReadInt("digest.maxCandidates", defaults.digestMaxCandidates), 20, 400);

        logs::info("config: radius={:.0f} cellOnly={} uniqueOnly={} includeDead={} maxConcurrent={} refinePass={}"sv,
                   g_settings.scanRadius, g_settings.cellOnly,
                   g_settings.uniqueOnly, g_settings.includeDead, g_settings.maxConcurrent,
                   g_settings.refinePass);
        logs::info("config: digest enabled={} autoBuild={} maxCandidates={}"sv,
                   g_settings.digestEnabled, g_settings.digestAutoBuild,
                   g_settings.digestMaxCandidates);
    }

    const Settings& Get() { return g_settings; }
}
