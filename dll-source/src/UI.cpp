#include "pch.h"

// SMF's header resolves everything through GetModuleHandle/GetProcAddress, the
// same runtime-binding style as SkyrimNet's. It is large and header-only, so it
// is included here and nowhere else. It calls std::filesystem without including
// it, which pch.h covers - keep pch.h first.
#include "SKSEMenuFramework.h"

#include "UI.h"
#include "Config.h"
#include "Generator.h"
#include "ScopeSelector.h"
#include "StagingStore.h"
#include "SkyrimNetAPI.h"

namespace BioForge::UI
{
    namespace
    {
        std::vector<Candidate> g_results;
        bool                   g_hasScanned = false;

        constexpr ImGuiMCP::ImVec4 kGapColour{ 1.00f, 0.72f, 0.30f, 1.0f };
        constexpr ImGuiMCP::ImVec4 kHaveColour{ 0.55f, 0.80f, 0.55f, 1.0f };

        // A generation is in flight for this reference if a snapshot entry is
        // still Generating - re-queuing the same NPC mid-flight would burn a
        // second LLM call for nothing.
        bool IsGenerating(std::uint32_t a_refFormID, const std::vector<Staging::Entry>& a_staged)
        {
            return std::any_of(a_staged.begin(), a_staged.end(), [&](const Staging::Entry& e) {
                return e.refFormID == a_refFormID && e.state == Staging::State::Generating;
            });
        }

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

        void DrawResults(const std::vector<Staging::Entry>& a_staged)
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
            if (!ImGuiMCP::BeginTable("bioforge_results", 7, flags, ImGuiMCP::ImVec2(0.0f, 400.0f))) {
                return;
            }

            ImGuiMCP::TableSetupColumn("Bio");
            ImGuiMCP::TableSetupColumn("Name");
            ImGuiMCP::TableSetupColumn("Reference");
            ImGuiMCP::TableSetupColumn("Source plugin");
            ImGuiMCP::TableSetupColumn("Dist");
            ImGuiMCP::TableSetupColumn("Template");
            ImGuiMCP::TableSetupColumn("Generate");
            ImGuiMCP::TableHeadersRow();

            for (const auto& c : g_results) {
                // ImGui identifies a widget by its LABEL, so every row's
                // "Generate" button would otherwise share one ID and only the
                // first row would ever fire. Scope each row by its reference.
                ImGuiMCP::PushID(static_cast<int>(c.refFormID));

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

                ImGuiMCP::TableNextColumn();
                if (IsGenerating(c.refFormID, a_staged)) {
                    ImGuiMCP::TextDisabled("working...");
                } else if (ImGuiMCP::Button("Generate")) {
                    Generator::Generate(c, g_results);
                }

                ImGuiMCP::PopID();
            }

            ImGuiMCP::EndTable();
        }

        // Review state. The text buffer is only refilled when the selection or
        // the underlying bundle changes - re-reading a 4KB file every frame for
        // a menu nobody is scrolling would be silly.
        std::uint32_t g_selected    = 0;
        std::string   g_reviewText;              // the bio as read from disk
        std::uint32_t g_bufferFor   = 0;
        bool          g_bufferIsRaw = false;
        bool          g_showRaw     = false;
        bool          g_openReader  = false;     // request to open the overlay

        void FillReviewBuffer(const Staging::Entry& a_entry, bool a_raw)
        {
            g_reviewText  = a_raw ? Staging::ReadRawResponse(a_entry)
                                  : Staging::ReadStagedBio(a_entry);
            g_bufferFor   = a_entry.refFormID;
            g_bufferIsRaw = a_raw;
        }

        const char* StateLabel(Staging::State a_state)
        {
            switch (a_state) {
            case Staging::State::Generating: return "working";
            case Staging::State::Staged:     return "staged";
            case Staging::State::Failed:     return "failed";
            }
            return "?";
        }

        // The reading surface. A bio is ~4KB of prose, which is unreadable in a
        // strip beside a table: this is a full-size modal that deliberately
        // covers the actor list, wraps rather than scrolling sideways, and
        // closes back to exactly where you were.
        void DrawReader(const std::vector<Staging::Entry>& a_staged)
        {
            constexpr auto kReaderTitle = "Bio Forge - review";

            if (g_openReader) {
                ImGuiMCP::OpenPopup(kReaderTitle);
                g_openReader = false;
            }

            ImGuiMCP::SetNextWindowSize(ImGuiMCP::ImVec2(900.0f, 660.0f),
                                        ImGuiMCP::ImGuiCond_Appearing);
            if (!ImGuiMCP::BeginPopupModal(kReaderTitle, nullptr, 0)) {
                return;
            }

            const Staging::Entry* sel = nullptr;
            for (const auto& e : a_staged) {
                if (e.refFormID == g_selected) {
                    sel = &e;
                    break;
                }
            }

            if (!sel) {   // discarded while open
                ImGuiMCP::CloseCurrentPopup();
                ImGuiMCP::EndPopup();
                return;
            }

            ImGuiMCP::Text("%s  ->  %s%s", sel->name.c_str(), sel->fileName.c_str(),
                           sel->committed ? "  (committed)" : "");
            if (!sel->note.empty()) {
                ImGuiMCP::TextWrapped("%s", sel->note.c_str());
            }
            ImGuiMCP::Separator();

            // A failed parse has no bio, so show what the model actually said -
            // that is the only way to see why it failed.
            const bool wantRaw = g_showRaw || sel->state == Staging::State::Failed;
            if (g_bufferFor != sel->refFormID || g_bufferIsRaw != wantRaw) {
                FillReviewBuffer(*sel, wantRaw);
            }

            // Reserve the last line for the buttons; the rest scrolls.
            if (ImGuiMCP::BeginChild("bioforge_reader_text", ImGuiMCP::ImVec2(0.0f, -36.0f))) {
                if (g_reviewText.empty()) {
                    ImGuiMCP::TextDisabled("(nothing to show)");
                } else {
                    ImGuiMCP::PushTextWrapPos(0.0f);   // wrap at the window edge
                    // "%s" matters: bio prose may contain a literal '%'.
                    ImGuiMCP::TextWrapped("%s", g_reviewText.c_str());
                    ImGuiMCP::PopTextWrapPos();
                }
            }
            ImGuiMCP::EndChild();

            ImGuiMCP::Separator();

            if (sel->state == Staging::State::Staged) {
                if (ImGuiMCP::Button(sel->committed ? "Commit again" : "Commit")) {
                    std::string note;
                    Staging::Commit(*sel, note);
                }
                ImGuiMCP::SameLine();
            }

            if (ImGuiMCP::Button("Regenerate")) {
                for (const auto& c : g_results) {
                    if (c.refFormID == sel->refFormID) {
                        Generator::Generate(c, g_results);
                        break;
                    }
                }
                g_bufferFor = 0;
            }
            ImGuiMCP::SameLine();

            if (ImGuiMCP::Button("Discard")) {
                Staging::Discard(*sel);
                g_selected  = 0;
                g_bufferFor = 0;
                ImGuiMCP::CloseCurrentPopup();
                ImGuiMCP::EndPopup();
                return;
            }
            ImGuiMCP::SameLine();

            if (ImGuiMCP::Button("Copy")) {
                ImGuiMCP::SetClipboardText(g_reviewText.c_str());
            }
            ImGuiMCP::SameLine();

            if (ImGuiMCP::Button(g_showRaw ? "Show bio" : "Show raw reply")) {
                g_showRaw   = !g_showRaw;
                g_bufferFor = 0;
            }
            ImGuiMCP::SameLine();

            if (ImGuiMCP::Button("Close")) {
                ImGuiMCP::CloseCurrentPopup();
            }

            ImGuiMCP::EndPopup();
        }

        void DrawReview(const std::vector<Staging::Entry>& a_staged)
        {
            ImGuiMCP::Separator();

            const auto progress = Generator::GetProgress();
            if (progress.Busy()) {
                ImGuiMCP::TextColored(kGapColour, "Generating: %d in flight, %d queued",
                                      progress.inFlight, progress.queued);
                if (progress.queued > 0) {
                    ImGuiMCP::SameLine();
                    if (ImGuiMCP::Button("Cancel queued")) {
                        Generator::CancelQueued();
                    }
                }
            } else {
                ImGuiMCP::Text("Review");
            }

            if (a_staged.empty()) {
                ImGuiMCP::TextDisabled("Nothing generated yet. Scan, then Generate.");
                return;
            }

            ImGuiMCP::TextDisabled("Select an NPC, then Read to review the bio.");

            if (ImGuiMCP::BeginChild("bioforge_list", ImGuiMCP::ImVec2(0.0f, 180.0f),
                                     ImGuiMCP::ImGuiChildFlags_Border)) {
                for (const auto& e : a_staged) {
                    ImGuiMCP::PushID(static_cast<int>(e.refFormID));

                    char label[192]{};
                    std::snprintf(label, sizeof(label), "%-28s %s%s", e.name.c_str(),
                                  StateLabel(e.state), e.committed ? ", committed" : "");

                    if (ImGuiMCP::Selectable(label, e.refFormID == g_selected)) {
                        g_selected = e.refFormID;
                    }
                    // Double-click straight into the reader, as a list should.
                    if (ImGuiMCP::IsItemHovered() && ImGuiMCP::IsMouseDoubleClicked(0)) {
                        g_selected   = e.refFormID;
                        g_openReader = true;
                    }

                    ImGuiMCP::PopID();
                }
            }
            ImGuiMCP::EndChild();

            const Staging::Entry* sel = nullptr;
            for (const auto& e : a_staged) {
                if (e.refFormID == g_selected) {
                    sel = &e;
                    break;
                }
            }

            const bool canRead = sel && sel->state != Staging::State::Generating;
            if (!canRead) {
                ImGuiMCP::BeginDisabled(true);
            }
            if (ImGuiMCP::Button("Read")) {
                g_openReader = true;
            }
            if (!canRead) {
                ImGuiMCP::EndDisabled();
            }

            if (sel && sel->state == Staging::State::Staged) {
                ImGuiMCP::SameLine();
                ImGuiMCP::PushID(static_cast<int>(sel->refFormID));
                if (ImGuiMCP::Button(sel->committed ? "Commit again" : "Commit")) {
                    std::string note;
                    Staging::Commit(*sel, note);
                }
                ImGuiMCP::PopID();
            }

            DrawReader(a_staged);
        }

        // SMF calls this on its own render thread while the game is paused, so
        // reading game data here is safe - but it must stay cheap. The scan and
        // generation only run on button presses, never per frame.
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

            if (!SN::CanGenerate()) {
                ImGuiMCP::TextColored(kGapColour,
                                      "Generation unavailable: SkyrimNet API v%d is older than v8.",
                                      SN::Version());
            }

            const auto staged = Staging::Snapshot();

            if (g_hasScanned) {
                std::vector<Candidate> gaps;
                for (const auto& c : g_results) {
                    if (c.IsGap()) {
                        gaps.push_back(c);
                    }
                }

                if (!gaps.empty()) {
                    char label[64]{};
                    std::snprintf(label, sizeof(label), "Generate all %zu missing bio(s)",
                                  gaps.size());
                    if (ImGuiMCP::Button(label)) {
                        Generator::GenerateAll(gaps, g_results);
                    }
                    ImGuiMCP::SameLine();
                    ImGuiMCP::TextDisabled("(%d at a time)", Config::Get().maxConcurrent);
                }
            }

            ImGuiMCP::Separator();
            DrawResults(staged);

            DrawReview(staged);
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
