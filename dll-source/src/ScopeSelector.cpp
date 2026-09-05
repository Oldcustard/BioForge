#include "pch.h"

#include "ScopeSelector.h"
#include "Config.h"
#include "SkyrimNetAPI.h"
#include "StagingStore.h"

namespace BioForge
{
    namespace
    {
        // Bios live under the game's Data tree; MO2's VFS resolves this to
        // whichever mod (usually overwrite) currently wins the file.
        constexpr auto kCharacterDir = "Data/SKSE/Plugins/SkyrimNet/prompts/characters"sv;

        bool BioFileExists(std::string_view a_template)
        {
            if (a_template.empty()) {
                return false;
            }

            std::string file{ a_template };
            if (!file.ends_with(".prompt")) {
                file += ".prompt";
            }

            std::error_code ec;   // non-throwing: a missing Data dir is not fatal
            const auto      path = std::filesystem::path{ kCharacterDir } / file;
            return std::filesystem::exists(path, ec);
        }

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

        std::size_t absent = 0;

        tes->ForEachReferenceInRange(player, cfg.scanRadius, [&](RE::TESObjectREFR* a_ref) {
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
            // They reach this callback at all because they are PERSISTENT, and
            // ForEachReferenceInRange walks the persistent list like any other.
            // Measured at Nightgate: Eriana, Caralia and Sangi - three mod
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
            if (cfg.cellOnly && a_ref->GetParentCell() != playerCell) {
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
            c.bioFileExists = BioFileExists(c.bioTemplate);
            c.trackedBySkyrimNet = SN::FormIDToUUID(c.refFormID) != 0;
            // Resolved once here rather than per frame: the panel renders this
            // for every row, and Render() must stay cheap.
            c.wouldWriteAs = Staging::BioFileName(c);

            out.push_back(std::move(c));
            return RE::BSContainer::ForEachResult::kContinue;
        });

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

        logs::info("--- Bio Forge scan: {} actor(s), {} without a bio ---"sv,
                   a_candidates.size(), gaps);

        for (const auto& c : a_candidates) {
            logs::info("  [{}] {:<28} ref={:08X} base={:08X} {:<24} race={:<18} dist={:>6.0f} bio='{}'"sv,
                       c.IsGap() ? "GAP " : "have",
                       c.name, c.refFormID, c.baseFormID, c.sourcePlugin, c.race, c.distance,
                       c.bioTemplate);
        }

        if (a_candidates.empty()) {
            logs::info("  (nothing matched - check scan.radius / scan.uniqueOnly / scan.cellOnly)"sv);
        }
    }
}
