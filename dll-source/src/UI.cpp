#include "pch.h"

// SMF's header resolves everything through GetModuleHandle/GetProcAddress, the
// same runtime-binding style as SkyrimNet's. It is large and header-only, so it
// is included here and nowhere else. It calls std::filesystem without including
// it, which pch.h covers - keep pch.h first.
#include "SKSEMenuFramework.h"

#include "UI.h"
#include "Config.h"
#include "ScopeSelector.h"
#include "SkyrimNetAPI.h"

namespace BioForge::UI
{
    namespace
    {
        std::vector<Candidate> g_results;
        bool                   g_hasScanned = false;

        constexpr ImGuiMCP::ImVec4 kGapColour{ 1.00f, 0.72f, 0.30f, 1.0f };
        constexpr ImGuiMCP::ImVec4 kHaveColour{ 0.55f, 0.80f, 0.55f, 1.0f };

        void DrawStatus()
        {
            if (!SN::Available()) {
                ImGuiMCP::TextColored(kGapColour, "SkyrimNet not found.");
                ImGuiMCP::TextWrapped("Bio Forge reads NPC data and generates bios through SkyrimNet. "
                                      "Install SkyrimNet and restart the game.");
                return;
            }

            ImGuiMCP::Text("SkyrimNet API v%d", SN::Version());
            if (!SN::MemorySystemReady()) {
                ImGuiMCP::TextDisabled("Database not ready - load a save first.");
            }

            const auto& cfg = Config::Get();
            ImGuiMCP::TextDisabled("Scope: %s, radius %.0f, %s NPCs%s",
                                   cfg.cellOnly ? "current cell" : "radius",
                                   cfg.scanRadius,
                                   cfg.uniqueOnly ? "unique" : "all",
                                   cfg.includeDead ? ", including dead" : "");
        }

        void DrawResults()
        {
            if (!g_hasScanned) {
                ImGuiMCP::TextDisabled("No scan yet.");
                return;
            }

            const auto gaps = std::count_if(g_results.begin(), g_results.end(),
                                            [](const Candidate& c) { return c.IsGap(); });

            ImGuiMCP::Text("%zu actor(s), %lld without a bio", g_results.size(),
                           static_cast<long long>(gaps));

            if (g_results.empty()) {
                ImGuiMCP::TextWrapped("Nothing matched. Widen the radius, turn off 'Current Cell Only', "
                                      "or turn off 'Unique NPCs Only' in SkyrimNet's BioForge settings.");
                return;
            }

            constexpr auto flags = ImGuiMCP::ImGuiTableFlags_RowBg | ImGuiMCP::ImGuiTableFlags_Borders |
                                   ImGuiMCP::ImGuiTableFlags_ScrollY;
            if (!ImGuiMCP::BeginTable("bioforge_results", 6, flags, ImGuiMCP::ImVec2(0.0f, 400.0f))) {
                return;
            }

            ImGuiMCP::TableSetupColumn("Bio");
            ImGuiMCP::TableSetupColumn("Name");
            ImGuiMCP::TableSetupColumn("Reference");
            ImGuiMCP::TableSetupColumn("Source plugin");
            ImGuiMCP::TableSetupColumn("Dist");
            ImGuiMCP::TableSetupColumn("Template");
            ImGuiMCP::TableHeadersRow();

            for (const auto& c : g_results) {
                ImGuiMCP::TableNextRow();

                ImGuiMCP::TableNextColumn();
                if (c.IsGap()) {
                    ImGuiMCP::TextColored(kGapColour, "missing");
                } else {
                    ImGuiMCP::TextColored(kHaveColour, "have");
                }

                ImGuiMCP::TableNextColumn();
                ImGuiMCP::Text("%s", c.name.c_str());

                ImGuiMCP::TableNextColumn();
                ImGuiMCP::Text("%08X", c.refFormID);

                ImGuiMCP::TableNextColumn();
                ImGuiMCP::Text("%s", c.sourcePlugin.c_str());

                ImGuiMCP::TableNextColumn();
                ImGuiMCP::Text("%.0f", c.distance);

                ImGuiMCP::TableNextColumn();
                if (c.bioTemplate.empty()) {
                    ImGuiMCP::TextDisabled("(none)");
                } else {
                    ImGuiMCP::Text("%s", c.bioTemplate.c_str());
                }
            }

            ImGuiMCP::EndTable();
        }

        // SMF calls this on its own render thread while the game is paused, so
        // reading game data here is safe - but it must stay cheap. The scan only
        // runs on the button press, never per frame.
        void __stdcall Render()
        {
            DrawStatus();
            ImGuiMCP::Separator();

            if (ImGuiMCP::Button("Scan for missing bios")) {
                g_results    = Scan();
                g_hasScanned = true;
                LogGapReport(g_results);
            }
            ImGuiMCP::SameLine();
            ImGuiMCP::TextDisabled("(also written to BioForge.log)");

            ImGuiMCP::Separator();
            DrawResults();
        }
    }

    void Register()
    {
        if (!SKSEMenuFramework::IsInstalled()) {
            logs::warn("SKSE Menu Framework not installed - Bio Forge has no UI"sv);
            return;
        }

        SKSEMenuFramework::SetSection("Bio Forge");
        SKSEMenuFramework::AddSectionItem("Scan", Render);
        logs::info("registered SMF page (open the mod control panel to use it)"sv);
    }
}
