#include "pch.h"

// SMF's header resolves everything through GetModuleHandle/GetProcAddress, the
// same runtime-binding style as SkyrimNet's. It is large and header-only, so it
// is included here and nowhere else. It calls std::filesystem without including
// it, which pch.h covers - keep pch.h first.
#include "SKSEMenuFramework.h"

#include "UI.h"
#include "Config.h"
#include "Generator.h"
#include "RegionDigest.h"
#include "ScopeSelector.h"
#include "StagingStore.h"
#include "SkyrimNetAPI.h"

namespace BioForge::UI
{
    namespace
    {
        std::vector<Candidate> g_results;
        bool                   g_hasScanned = false;

        // The cell the scan was taken in. A scan is a snapshot of one room:
        // its rows carry distances from where the player stood, and its
        // Generate buttons point at actors who may now be a load door away.
        // Kept as a FormID rather than a pointer - an unloaded cell would
        // leave a dangling one.
        std::uint32_t g_scanCell = 0;

        std::uint32_t CurrentCellID()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* cell   = player ? player->GetParentCell() : nullptr;
            return cell ? cell->GetFormID() : 0;
        }

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

        // The region line. Worth its own row because the digest is the one
        // piece of context that is shared, cached and expensive - the user
        // should be able to see whether the bios they are about to generate
        // will have it, before spending the calls.
        // Set by the View button, consumed next frame to open the modal - the
        // same one-shot the bio reader uses.
        bool g_openDigestReader = false;

        // The digest is the one piece of context that is shared, cached and
        // paid for once, so being able to read the thing before spending a
        // batch against it matters more than for any single bio. It is on disk
        // as plain text, but nobody should have to go and find the file.
        void DrawDigestReader(const std::string& a_region, const std::string& a_digest)
        {
            constexpr auto kDigestTitle = "Bio Forge - regional digest";

            if (g_openDigestReader) {
                ImGuiMCP::OpenPopup(kDigestTitle);
                g_openDigestReader = false;
            }

            ImGuiMCP::SetNextWindowSize(ImGuiMCP::ImVec2(760.0f, 520.0f),
                                        ImGuiMCP::ImGuiCond_Appearing);
            if (!ImGuiMCP::BeginPopupModal(kDigestTitle, nullptr, 0)) {
                return;
            }

            ImGuiMCP::Text("%s", a_region.c_str());
            ImGuiMCP::TextDisabled("Injected into every bio generated here.");
            ImGuiMCP::Separator();

            // Reserve the last line for the buttons; the rest scrolls.
            if (ImGuiMCP::BeginChild("bioforge_digest_text", ImGuiMCP::ImVec2(0.0f, -36.0f))) {
                if (a_digest.empty()) {
                    ImGuiMCP::TextDisabled("(nothing cached)");
                } else {
                    ImGuiMCP::PushTextWrapPos(0.0f);
                    // "%s" matters: a digest line may contain a literal '%'.
                    ImGuiMCP::TextWrapped("%s", a_digest.c_str());
                    ImGuiMCP::PopTextWrapPos();
                }
            }
            ImGuiMCP::EndChild();

            ImGuiMCP::Separator();
            if (ImGuiMCP::Button("Copy")) {
                ImGuiMCP::SetClipboardText(a_digest.c_str());
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Close")) {
                ImGuiMCP::CloseCurrentPopup();
            }
            ImGuiMCP::EndPopup();
        }

        void DrawRegion()
        {
            const auto& cfg = Config::Get();
            if (!cfg.digestEnabled) {
                ImGuiMCP::TextDisabled("Regional digest off - bios use each NPC's own evidence only.");
                return;
            }

            const auto region = RegionDigest::Current();
            if (!region.Valid()) {
                ImGuiMCP::TextDisabled("Region: (none here) - no digest out in the wilds.");
                return;
            }

            const bool building = RegionDigest::Building();
            const auto digest   = RegionDigest::Get(region.name);

            if (region.hold.empty()) {
                ImGuiMCP::Text("Region: %s", region.name.c_str());
            } else {
                ImGuiMCP::Text("Region: %s (%s)", region.name.c_str(), region.hold.c_str());
            }
            ImGuiMCP::SameLine();

            if (building) {
                ImGuiMCP::TextColored(kGapColour, "- digest building...");
                return;
            }
            if (digest.empty()) {
                ImGuiMCP::TextColored(kGapColour, "- no digest");
            } else {
                ImGuiMCP::TextColored(kHaveColour, "- digest ready");
            }

            if (!digest.empty()) {
                ImGuiMCP::SameLine();
                if (ImGuiMCP::Button("View digest")) {
                    g_openDigestReader = true;
                }
            }

            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button(digest.empty() ? "Build digest" : "Rebuild digest")) {
                RegionDigest::Build(region, !digest.empty());
            }

            DrawDigestReader(region.name, digest);
        }

        void DrawResults(const std::vector<Staging::Entry>& a_staged)
        {
            if (!g_hasScanned) {
                ImGuiMCP::TextDisabled("No scan yet.");
                return;
            }

            const auto gaps = std::count_if(g_results.begin(), g_results.end(),
                                            [](const Candidate& c) { return c.IsGap(); });

            const auto blocked = std::count_if(g_results.begin(), g_results.end(),
                                               [](const Candidate& c) {
                                                   return c.IsGap() && !c.CanGenerate();
                                               });

            ImGuiMCP::Text("%zu actor(s), %lld without a bio", g_results.size(),
                           static_cast<long long>(gaps));
            if (blocked > 0) {
                ImGuiMCP::SameLine();
                ImGuiMCP::TextDisabled("- %lld of them not tracked by SkyrimNet",
                                       static_cast<long long>(blocked));
                if (ImGuiMCP::IsItemHovered()) {
                    ImGuiMCP::SetTooltip(
                        "SkyrimNet has never registered these actors, so it has no UUID for\n"
                        "them and a generation cannot be dispatched. Talk to them, or get\n"
                        "closer, then scan again.");
                }
            }

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
                if (!c.bioTemplate.empty()) {
                    ImGuiMCP::Text("%s", c.bioTemplate.c_str());
                } else if (!c.CanGenerate()) {
                    // Showing the derived name here would promise something
                    // that cannot happen: with no UUID there is nothing to
                    // generate, so there will never be a bio to commit.
                    ImGuiMCP::TextDisabled("not tracked");
                    if (ImGuiMCP::IsItemHovered()) {
                        ImGuiMCP::SetTooltip(
                            "SkyrimNet has not registered this actor, so it has neither a\n"
                            "template name nor a UUID. Nothing can be generated until it does.");
                    }
                } else {
                    // An empty template is not the same as no bio: SkyrimNet
                    // assigns the name when it first registers an actor, and
                    // writes the file later, so "missing" rows come both with
                    // a name and without one. "(none)" reported the absence
                    // and left the reader to guess the consequence. Show the
                    // name a commit would derive instead - dimmed and marked,
                    // because it is our fallback rather than SkyrimNet's word.
                    ImGuiMCP::TextDisabled("%s *", c.wouldWriteAs.c_str());
                    if (ImGuiMCP::IsItemHovered()) {
                        ImGuiMCP::SetTooltip(
                            "SkyrimNet has not assigned this actor a template name.\n"
                            "Committing would write %s.prompt.",
                            c.wouldWriteAs.c_str());
                    }
                }

                ImGuiMCP::TableNextColumn();
                if (IsGenerating(c.refFormID, a_staged)) {
                    ImGuiMCP::TextDisabled("working...");
                } else if (!c.CanGenerate()) {
                    // The button used to be here and did nothing at all: the
                    // job was dropped in BuildContext for want of a UUID, and
                    // the only trace was a line in the log.
                    ImGuiMCP::TextDisabled("unavailable");
                    if (ImGuiMCP::IsItemHovered()) {
                        ImGuiMCP::SetTooltip(
                            "SkyrimNet is not tracking this actor, so there is no UUID to\n"
                            "generate against. Talk to them or get closer, then scan again.");
                    }
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
        bool          g_openDiscardAll = false;  // ditto, for the bulk confirm

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

            const bool drop = ImGuiMCP::Button(sel->committed ? "Dismiss" : "Discard");
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    sel->committed
                        ? "Take it off the review list. The committed bio stays where it is."
                        : "Delete the staged bio. It has not been committed, so this is the "
                          "only copy.");
            }
            if (drop) {
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

        // Discarding one bio is a small mistake; discarding a whole batch is
        // a dozen LLM calls, so this one asks. The count of what is actually
        // at risk is in the question, because "committed bios stay on disk" is
        // the half people forget.
        void DrawDiscardAllConfirm(const std::vector<Staging::Entry>& a_staged,
                                   std::ptrdiff_t                     a_uncommitted)
        {
            constexpr auto kConfirmTitle = "Bio Forge - discard all?";

            if (g_openDiscardAll) {
                ImGuiMCP::OpenPopup(kConfirmTitle);
                g_openDiscardAll = false;
            }
            if (!ImGuiMCP::BeginPopupModal(kConfirmTitle, nullptr, 0)) {
                return;
            }

            if (a_uncommitted > 0) {
                ImGuiMCP::TextColored(kGapColour, "%lld staged bio(s) have not been committed.",
                                      static_cast<long long>(a_uncommitted));
                ImGuiMCP::Text("Discarding them deletes the only copy there is.");
            } else {
                ImGuiMCP::Text("Nothing here is uncommitted.");
            }
            ImGuiMCP::TextDisabled("Bios already committed stay in prompts/characters.");
            ImGuiMCP::Separator();

            if (ImGuiMCP::Button("Discard them")) {
                std::size_t dropped = 0;
                for (const auto& e : a_staged) {
                    // A generation still in flight keeps its entry: the
                    // completion callback has to find it.
                    if (e.state == Staging::State::Generating) {
                        continue;
                    }
                    Staging::Discard(e);
                    ++dropped;
                }
                g_selected  = 0;
                g_bufferFor = 0;
                logs::info("staging: discarded {} entr{} in one press"sv, dropped,
                           dropped == 1 ? "y" : "ies");
                ImGuiMCP::CloseCurrentPopup();
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Keep them")) {
                ImGuiMCP::CloseCurrentPopup();
            }
            ImGuiMCP::EndPopup();
        }

        void DrawReview(const std::vector<Staging::Entry>& a_staged)
        {
            ImGuiMCP::Separator();

            const auto progress = Generator::GetProgress();
            if (progress.Busy()) {
                if (progress.pending > 0) {
                    ImGuiMCP::TextColored(kGapColour,
                                          "Waiting on the %s digest: %d bio(s) queued behind it",
                                          progress.pendingRegion.c_str(), progress.pending);
                } else if (progress.revising > 0) {
                    // Pass two, running. Calling this "generating" was a lie
                    // twice over: nothing new is being written, and the counts
                    // are of relationship blocks rather than bios.
                    // A refine pass only starts once nothing else is in
                    // flight, so during one every job of either count is a
                    // refine and the plain totals are the honest ones.
                    ImGuiMCP::TextColored(kGapColour, "Revising ties: %d in flight, %d queued",
                                          progress.inFlight, progress.queued);
                } else if (progress.awaitingRevision > 0) {
                    // The refine pass is not a footnote: with it off, a batch's
                    // NPCs know nothing about each other. Saying so stops the
                    // panel looking finished while a second pass is still due.
                    ImGuiMCP::TextColored(
                        kGapColour, "Generating: %d in flight, %d queued, %d awaiting revision",
                        progress.inFlight, progress.queued, progress.awaitingRevision);
                } else {
                    ImGuiMCP::TextColored(kGapColour, "Generating: %d in flight, %d queued",
                                          progress.inFlight, progress.queued);
                }
                if (progress.queued > 0 || progress.pending > 0) {
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
                    std::snprintf(label, sizeof(label), "%-28s %s%s%s", e.name.c_str(),
                                  StateLabel(e.state), e.refined ? ", ties revised" : "",
                                  e.committed ? ", committed" : "");

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

            // Two words, because the consequences are not the same one.
            // Discarding an uncommitted bio destroys the only copy there is;
            // dismissing a committed one only stops listing finished work, and
            // prompts/characters is untouched either way. Calling both of them
            // "Discard" - and only offering it inside the reader - is why a
            // committed entry looked like it was stuck in the list for good.
            // Never offered mid-generation: the completion callback still has
            // to find its entry.
            if (sel && sel->state != Staging::State::Generating) {
                ImGuiMCP::SameLine();
                ImGuiMCP::PushID(static_cast<int>(sel->refFormID));
                const bool drop = ImGuiMCP::Button(sel->committed ? "Dismiss" : "Discard");
                if (ImGuiMCP::IsItemHovered()) {
                    ImGuiMCP::SetTooltip(
                        sel->committed
                            ? "Take it off this list. The committed bio stays where it is."
                            : "Delete the staged bio. It has not been committed, so this is "
                              "the only copy.");
                }
                if (drop) {
                    Staging::Discard(*sel);
                    g_selected  = 0;
                    g_bufferFor = 0;
                }
                ImGuiMCP::PopID();
            }

            // --- bulk actions, on their own row ---
            //
            // A batch is the normal unit of work here: you generate eight and
            // then agree with eight, or with none. Doing that one selection at
            // a time is the tedium this panel exists to remove.
            const auto committed = std::count_if(
                a_staged.begin(), a_staged.end(),
                [](const Staging::Entry& e) { return e.committed; });
            const auto uncommitted = std::count_if(
                a_staged.begin(), a_staged.end(), [](const Staging::Entry& e) {
                    return e.state == Staging::State::Staged && !e.committed;
                });
            const auto removable = std::count_if(
                a_staged.begin(), a_staged.end(), [](const Staging::Entry& e) {
                    return e.state != Staging::State::Generating;
                });

            if (uncommitted > 0) {
                char label[48]{};
                std::snprintf(label, sizeof(label), "Commit all (%lld)",
                              static_cast<long long>(uncommitted));
                const bool commitAll = ImGuiMCP::Button(label);
                if (ImGuiMCP::IsItemHovered()) {
                    ImGuiMCP::SetTooltip(
                        "Write every staged bio that has not been committed yet.\n"
                        "Already-committed entries are left alone rather than "
                        "re-committed.");
                }
                if (commitAll) {
                    std::size_t done = 0;
                    for (const auto& e : a_staged) {
                        if (e.state != Staging::State::Staged || e.committed) {
                            continue;
                        }
                        std::string note;
                        if (Staging::Commit(e, note)) {
                            ++done;
                        }
                    }
                    logs::info("commit: committed {} of {} staged bio(s) in one press"sv,
                               done, static_cast<std::size_t>(uncommitted));
                }
                ImGuiMCP::SameLine();
            }

            if (removable > 0) {
                char label[48]{};
                std::snprintf(label, sizeof(label), "Discard all (%lld)",
                              static_cast<long long>(removable));
                if (ImGuiMCP::Button(label)) {
                    // Confirmed, because uncommitted bios are an LLM call each
                    // and this is one press away from destroying all of them.
                    g_openDiscardAll = true;
                }
                if (ImGuiMCP::IsItemHovered()) {
                    ImGuiMCP::SetTooltip(
                        "Empty the review list. Anything not committed is gone;\n"
                        "committed bios stay on disk. Asks first.");
                }
                ImGuiMCP::SameLine();
            }

            if (committed > 0) {
                char label[48]{};
                std::snprintf(label, sizeof(label), "Clear committed (%lld)",
                              static_cast<long long>(committed));
                const bool clear = ImGuiMCP::Button(label);
                if (ImGuiMCP::IsItemHovered()) {
                    ImGuiMCP::SetTooltip(
                        "Drop every committed entry from the list at once.\n"
                        "The bios themselves are already written and are left alone.");
                }
                if (clear) {
                    // a_staged is this frame's snapshot, not the store, so
                    // discarding while walking it is safe.
                    for (const auto& e : a_staged) {
                        if (!e.committed) {
                            continue;
                        }
                        if (e.refFormID == g_selected) {
                            g_selected  = 0;
                            g_bufferFor = 0;
                        }
                        Staging::Discard(e);
                    }
                }
            }

            DrawDiscardAllConfirm(a_staged, uncommitted);
            DrawReader(a_staged);
        }

        // SMF calls this on its own render thread while the game is paused, so
        // reading game data here is safe - but it must stay cheap. The scan and
        // generation only run on button presses, never per frame.
        void __stdcall Render()
        {
            // The plugin's only main-thread pump. A batch held for its region
            // digest is released here, so it advances only while this panel is
            // open - which is where you pressed the button and are watching it.
            Generator::Tick();

            // Settings are re-read here, so changing one in SkyrimNet's panel
            // takes effect on the next frame rather than the next launch, and
            // without the user having to ask for it. Throttled inside Refresh()
            // and silent unless something moved. Skipped mid-batch because Pump
            // reads maxConcurrent from a completion callback on a worker
            // thread; a batch keeps the settings it started with either way.
            if (!Generator::GetProgress().Busy()) {
                Config::Refresh();

                // Drop a scan the player has walked out of, rather than leave
                // rows describing a room they have left. Not while a batch is
                // live: the roster was snapshotted when it started, and the
                // reader's Regenerate resolves its candidate out of this list.
                if (g_hasScanned && CurrentCellID() != g_scanCell) {
                    g_results.clear();
                    g_hasScanned = false;
                    g_scanCell   = 0;
                }
            }

            DrawStatus();
            if (SN::Available()) {
                DrawRegion();
            }
            ImGuiMCP::Separator();

            if (ImGuiMCP::Button("Scan for missing bios")) {
                g_results    = Scan();
                g_hasScanned = true;
                g_scanCell   = CurrentCellID();
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
                // Untracked actors are filtered out HERE rather than inside
                // the batch, so the button's count is what will actually be
                // generated. It used to promise six and quietly deliver three.
                std::vector<Candidate> gaps;
                std::size_t            blocked = 0;
                for (const auto& c : g_results) {
                    if (!c.IsGap()) {
                        continue;
                    }
                    if (c.CanGenerate()) {
                        gaps.push_back(c);
                    } else {
                        ++blocked;
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
                if (blocked > 0) {
                    ImGuiMCP::TextDisabled(
                        "%zu more cannot be generated: SkyrimNet is not tracking them.",
                        blocked);
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
