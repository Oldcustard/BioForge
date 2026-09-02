#include "pch.h"

#include "Config.h"
#include "SkyrimNetAPI.h"

#include <charconv>

namespace BioForge::Config
{
    namespace
    {
        Settings g_settings{};

        // SkyrimNet hands config values back as strings. Anything we cannot parse
        // keeps the compiled-in default rather than silently becoming zero.
        std::uint32_t ReadUInt(const char* a_path, std::uint32_t a_fallback)
        {
            const auto raw = SN::PluginConfigValue("BioForge", a_path, "");
            if (raw.empty()) {
                return a_fallback;
            }
            std::uint32_t out{};
            const auto*   first = raw.data();
            const auto*   last  = raw.data() + raw.size();
            const int     base  = raw.starts_with("0x") || raw.starts_with("0X") ? (first += 2, 16) : 10;
            const auto    res   = std::from_chars(first, last, out, base);
            return res.ec == std::errc{} ? out : a_fallback;
        }

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
        g_settings.scanHotkey  = ReadUInt("scan.hotkey", defaults.scanHotkey);
        g_settings.scanRadius  = ReadFloat("scan.radius", defaults.scanRadius);
        g_settings.cellOnly    = ReadBool("scan.cellOnly", defaults.cellOnly);
        g_settings.uniqueOnly  = ReadBool("scan.uniqueOnly", defaults.uniqueOnly);
        g_settings.includeDead = ReadBool("scan.includeDead", defaults.includeDead);

        logs::info("config: hotkey=0x{:02X} radius={:.0f} cellOnly={} uniqueOnly={} includeDead={}"sv,
                   g_settings.scanHotkey, g_settings.scanRadius,
                   g_settings.cellOnly, g_settings.uniqueOnly, g_settings.includeDead);
    }

    const Settings& Get() { return g_settings; }
}
