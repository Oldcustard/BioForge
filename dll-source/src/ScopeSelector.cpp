#include "pch.h"

#include "ScopeSelector.h"
#include "Config.h"
#include "ContentLibrary.h"
#include "SkyrimNetAPI.h"
#include "StagingStore.h"

namespace BioForge
{
    namespace
    {
        std::string SourcePluginOf(const RE::TESForm* a_form)
        {
            if (!a_form) {
                return {};
            }
            const auto* file = a_form->GetFile(0);
            return file ? std::string{ file->GetFilename() } : std::string{};
        }
    }

    std::vector<Candidate> Scan()
    {
        std::vector<Candidate> out;

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* tes    = RE::TES::GetSingleton();
        if (!player || !tes) {
            logs::warn("scan: no player or TES singleton"sv);
            return out;
        }

        const auto& cfg        = Config::Get();
        const auto* playerCell = player->GetParentCell();

        // Scope follows where the player is. Indoors the scan is the whole
        // current cell, however large, so it enumerates the cell directly and
        // needs no radius - a Blue-Palace-sized hall is covered without a
        // number to guess. Outdoors there is no single cell to bound it, so it
        // sweeps a radius and crosses cell boundaries as it goes.
        const bool interior = playerCell && playerCell->IsInteriorCell();

        std::size_t absent = 0;

        // One content-library sweep per scan, not per row: every layer
        // SkyrimNet resolves through (hub plugins, external layers, the
        // player's overlay, this playthrough's per-save files) is walked
        // once and each candidate then consults the index. Beta 24 stat'ed
        // a single prompts/characters directory; Beta 25 spreads those
        // files across layers.
        ContentLibrary::Index library;
        library.Build();

        auto perRef = [&](RE::TESObjectREFR* a_ref) -> RE::BSContainer::ForEachResult {
            if (!a_ref) {
                return RE::BSContainer::ForEachResult::kContinue;
            }

            auto* actor = a_ref->As<RE::Actor>();
            if (!actor || actor == player) {
                return RE::BSContainer::ForEachResult::kContinue;
            }

            // A DISABLED reference is not in the world: no 3D, nobody can see
            // or talk to it, and SkyrimNet never registers it - so it has no
            // UUID, and a generation dispatched for it is dropped on the floor.
            // They reach this callback at all because they are PERSISTENT, so
            // the cell's reference list holds them like any other - both
            // enumerators below surface them. Measured at Nightgate: Eriana,
            // Caralia and Sangi - three mod
            // followers parked as Persistent + InitiallyDisabled until they are
            // recruited - were counted as gaps and turned "Generate all 6" into
            // three bios. Tested at RUNTIME, not on the record flag, so an NPC
            // who has since been enabled is scanned normally.
            if (a_ref->IsDisabled() || a_ref->IsDeleted()) {
                ++absent;
                return RE::BSContainer::ForEachResult::kContinue;
            }
            if (!cfg.includeDead && actor->IsDead()) {
                return RE::BSContainer::ForEachResult::kContinue;
            }

            // NOTE: for LEVELED actors GetActorBase() is the template shell, whose
            // race/sex fields are junk. Harmless here because uniqueOnly filters
            // them out - unique NPCs are never templated - but do not trust race
            // on a non-unique row.
            auto* base = actor->GetActorBase();
            if (!base) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            if (cfg.uniqueOnly && !base->IsUnique()) {
                return RE::BSContainer::ForEachResult::kContinue;
            }

            const char* display = actor->GetDisplayFullName();
            if (!display || !*display) {
                return RE::BSContainer::ForEachResult::kContinue;   // unnamed: no bio key possible
            }

            Candidate c{};
            c.refFormID    = a_ref->GetFormID();
            c.baseFormID   = base->GetFormID();
            c.name         = display;
            c.race         = base->GetRace() ? base->GetRace()->GetFormEditorID() : "";
            c.sourcePlugin = SourcePluginOf(base);
            c.isUnique     = base->IsUnique();
            c.isFollower   = actor->IsPlayerTeammate();
            const auto pos = a_ref->GetPosition();
            c.posX         = pos.x;
            c.posY         = pos.y;
            c.posZ         = pos.z;
            c.distance     = player->GetPosition().GetDistance(pos);
            c.bioTemplate  = SN::BioTemplateName(c.refFormID);
            c.trackedBySkyrimNet = SN::FormIDToUUID(c.refFormID) != 0;
            // Resolved once here rather than per frame: the panel renders this
            // for every row, and Render() must stay cheap.
            c.wouldWriteAs = Staging::BioFileName(c);

            // Detection asks the same question SkyrimNet's renderer asks:
            // does any layer provide a bio under this template stem. A
            // per-save DYNAMIC bio counts as covered and is flagged as such -
            // generating over one would throw away characterisation this
            // playthrough has evolved (and, since SkyrimNet renders the
            // dynamic copy first, the new static bio would lose to it
            // anyway).
            if (const auto* bio = library.Find(c.wouldWriteAs)) {
                c.bioFileExists = true;
                c.bioDynamic    = bio->dynamic;
            }

            out.push_back(std::move(c));
            return RE::BSContainer::ForEachResult::kContinue;
        };

        if (interior) {
            playerCell->ForEachReference(perRef);
        } else {
            tes->ForEachReferenceInRange(player, cfg.exteriorScanRadius, perRef);
        }

        if (absent > 0) {
            // Logged rather than silent: this is the difference between what
            // the cell record holds and who is actually standing in it.
            logs::info("scan: skipped {} disabled reference(s) - present in the cell"
                       " record, not in the world"sv,
                       absent);
        }

        std::sort(out.begin(), out.end(),
                  [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
        return out;
    }

    void LogGapReport(const std::vector<Candidate>& a_candidates)
    {
        const auto gaps = std::count_if(a_candidates.begin(), a_candidates.end(),
                                        [](const Candidate& c) { return c.IsGap(); });

        logs::info("--- BioForge scan: {} actor(s), {} without a bio ---"sv,
                   a_candidates.size(), gaps);

        for (const auto& c : a_candidates) {
            logs::info("  [{}] {:<28} ref={:08X} base={:08X} {:<24} race={:<18} dist={:>6.0f} bio='{}'"sv,
                       c.IsGap() ? "GAP " : c.bioDynamic ? "dyn " : "have",
                       c.name, c.refFormID, c.baseFormID, c.sourcePlugin, c.race, c.distance,
                       c.bioTemplate);
        }

        if (a_candidates.empty()) {
            logs::info("  (nothing matched - check scan.exteriorRadius / scan.uniqueOnly)"sv);
        }
    }
}
