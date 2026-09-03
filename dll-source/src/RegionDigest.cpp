#include "pch.h"

#include "RegionDigest.h"

#include "Config.h"
#include "Json.h"
#include "SkyrimNetAPI.h"
#include "StagingStore.h"

#include <atomic>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>

namespace BioForge::RegionDigest
{
    namespace
    {
        constexpr auto kPromptName = "bioforge_region_digest"sv;

        // SkyrimNet's own profile-writing variant, for the same reason the bio
        // generator uses it: one model choice for the user, and tuning that
        // suits prose rather than the dialogue defaults. See Generator.cpp.
        constexpr auto kVariant = "CharacterProfileGeneration"sv;

        // Location keywords marking a place people LIVE in, as opposed to one
        // building. Deliberately not LocTypeHabitation: that sits on inns and
        // farms too, and stopping the walk there would key the digest to a
        // single tavern.
        constexpr std::string_view kSettlementTypes[] = {
            "LocTypeCity"sv, "LocTypeTown"sv, "LocTypeSettlement"sv
        };
        constexpr auto kHoldType = "LocTypeHold"sv;

        // Below this many bios naming the settlement itself, the harvest is
        // topped up from the surrounding hold - otherwise a hamlet hands the
        // model almost nobody to choose from.
        constexpr std::size_t kMinPrimary = 25;

        std::mutex                                      g_mutex;
        std::map<std::string, std::string, std::less<>> g_cache;
        std::atomic<bool>                               g_building{ false };

        std::filesystem::path CacheDir()
        {
            // Beside the DLL, NOT under prompts/: SkyrimNet scans that tree for
            // templates, and these are data files.
            return Staging::PromptsDir().parent_path().parent_path() / "BioForge" / "regions";
        }

        std::string Sanitise(std::string_view a_name)
        {
            std::string out;
            for (const char ch : a_name) {
                out += (std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_');
            }
            return out.empty() ? "unknown" : out;
        }

        std::string ReadFile(const std::filesystem::path& a_path)
        {
            std::ifstream in{ a_path, std::ios::binary };
            if (!in) {
                return {};
            }
            return std::string{ std::istreambuf_iterator<char>{ in },
                                std::istreambuf_iterator<char>{} };
        }

        // "torg_strong-arm_9A8" -> "Torg Strong-Arm". The corpus key is the
        // display name lowercased with spaces as underscores, plus a reference
        // suffix.
        //
        // The uppercasing is written out rather than calling toupper. An
        // ASCII fold is exactly right here and carries no locale surprises:
        // BioFileName builds these stems by stripping everything outside
        // [a-z0-9_-], so there is nothing else in them to fold.
        std::string NameFromStem(std::string a_stem)
        {
            const auto underscore = a_stem.find_last_of('_');
            if (underscore != std::string::npos && a_stem.size() - underscore <= 5) {
                a_stem.resize(underscore);   // drop the _9A8 reference suffix
            }

            std::string out;
            bool        capitalise = true;
            for (const char ch : a_stem) {
                const char c = (ch == '_') ? ' ' : ch;
                out += (capitalise && c >= 'a' && c <= 'z')
                           ? static_cast<char>(c - 'a' + 'A')
                           : c;
                capitalise = (c == ' ' || c == '-');
            }
            return out;
        }

        std::string FirstSentence(std::string_view a_text, std::size_t a_maxChars)
        {
            const auto stop = a_text.find(". ");
            auto       cut  = stop == std::string_view::npos ? a_text.size() : stop + 1;
            // (std::min) parenthesized: windows.h defines min as a macro, and
            // the bare call expands wrongly (C2589). This is the one that
            // actually blocked this file from compiling.
            cut             = (std::min)(cut, a_maxChars);

            std::string out{ a_text.substr(0, cut) };
            for (auto& ch : out) {
                if (ch == '\n' || ch == '\r') {
                    ch = ' ';
                }
            }
            return out;
        }

        // Keep only the bullet lines. The prompt asks for bullets and nothing
        // else, but models still open with "Here is the reference sheet:", and
        // this text rides along on EVERY bio generated in the region - a wasted
        // line is paid for hundreds of times. It also guarantees no line starts
        // with '[', which would cost the enclosing heading in the bio prompt.
        std::string KeepBullets(std::string_view a_raw)
        {
            std::string out;
            std::size_t pos = 0;
            while (pos <= a_raw.size()) {
                const auto end  = a_raw.find('\n', pos);
                auto       line = a_raw.substr(pos, end == std::string_view::npos
                                                        ? std::string_view::npos
                                                        : end - pos);
                pos = end == std::string_view::npos ? a_raw.size() + 1 : end + 1;

                while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
                    line.remove_prefix(1);
                }
                while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) {
                    line.remove_suffix(1);
                }

                if (line.starts_with("- ") || line.starts_with("* ")) {
                    out += "- ";
                    out += line.substr(2);
                    out += '\n';
                }
            }
            if (!out.empty()) {
                out.pop_back();
            }
            return out;
        }

        bool IsSettlement(const RE::BGSLocation* a_location)
        {
            return std::any_of(std::begin(kSettlementTypes), std::end(kSettlementTypes),
                               [&](std::string_view a_keyword) {
                                   return a_location->HasKeywordString(a_keyword);
                               });
        }

        std::string NameOf(const RE::BGSLocation* a_location)
        {
            const char* name = a_location ? a_location->GetFullName() : nullptr;
            return name ? std::string{ name } : std::string{};
        }

        // Existing bios that name this place. The candidate source is the
        // user's OWN installed corpus: each bio's summary block is a ready-made
        // description, and the set reflects their actual load order, mod-added
        // characters included.
        std::string GatherCandidates(const Region& a_region, std::size_t a_max)
        {
            const auto      dir = Staging::PromptsDir() / "characters";
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) {
                logs::warn("digest: no bio corpus at {}"sv, dir.string());
                return {};
            }

            static const std::regex summaryRe{
                R"(\{%\s*block\s+summary\s*%\}([\s\S]*?)\{%\s*endblock)"
            };

            // Two tiers. A bio naming the settlement is direct evidence about
            // this place; one naming only the hold is the fallback for a hamlet
            // whose own name almost nobody mentions. Keeping them apart is the
            // whole point of narrowing the key - topping up from the hold must
            // never crowd out the settlement's own cast.
            std::vector<std::string> primary;
            std::vector<std::string> secondary;

            // Bounded: a corpus can run to thousands of files, and there is no
            // reason to read them all once the answer is in hand.
            const std::size_t ceiling = a_max * 2;

            for (const auto& file : std::filesystem::directory_iterator{ dir, ec }) {
                if (primary.size() >= a_max ||
                    primary.size() + secondary.size() >= ceiling) {
                    break;
                }
                if (!file.is_regular_file() || file.path().extension() != ".prompt") {
                    continue;   // also skips the .prompt.backup.<time> files
                }

                const auto text = ReadFile(file.path());
                if (text.empty()) {
                    continue;
                }

                const bool namesRegion = text.find(a_region.name) != std::string::npos;
                const bool namesHold   = !a_region.hold.empty() &&
                                       text.find(a_region.hold) != std::string::npos;
                if (!namesRegion && !namesHold) {
                    continue;
                }

                std::smatch match;
                if (!std::regex_search(text, match, summaryRe)) {
                    continue;
                }

                auto line = "- " + NameFromStem(file.path().stem().string()) + ": " +
                            FirstSentence(match[1].str(), 240) + "\n";
                (namesRegion ? primary : secondary).push_back(std::move(line));
            }

            std::string out;
            for (const auto& line : primary) {
                out += line;
            }

            std::size_t toppedUp = 0;
            if (primary.size() < kMinPrimary) {
                for (const auto& line : secondary) {
                    if (primary.size() + toppedUp >= a_max) {
                        break;
                    }
                    out += line;
                    ++toppedUp;
                }
            }

            logs::info("digest: {} - {} bio(s) name it, {} topped up from {}"sv,
                       a_region.name, primary.size(), toppedUp,
                       a_region.hold.empty() ? std::string{ "(no hold)" } : a_region.hold);
            return out;
        }
    }

    Region Current()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return {};
        }

        const auto* location = player->GetCurrentLocation();
        if (!location) {
            return {};   // open wilderness carries no location record
        }

        const RE::BGSLocation* settlement = nullptr;
        const RE::BGSLocation* hold       = nullptr;
        const RE::BGSLocation* top        = location;

        // interior -> city -> hold. Stop at the SETTLEMENT rather than walking
        // to the top of the tree: the top is the hold, which is what an earlier
        // cut keyed on and it was far too coarse. Bounded in case a mod ever
        // makes the parent chain circular.
        for (int hop = 0; location && hop < 8; ++hop) {
            if (!settlement && IsSettlement(location)) {
                settlement = location;
            }
            if (!hold && location->HasKeywordString(kHoldType)) {
                hold = location;
            }
            top      = location;
            location = location->parentLoc;
        }

        Region region;
        // Some mod settlements parent straight to a hold, or to nothing at all;
        // those fall back to the hold, which is no worse than the old key.
        region.name = NameOf(settlement ? settlement : (hold ? hold : top));
        region.hold = NameOf(hold);

        // Winterhold is both a hold and its own town, and a mod settlement may
        // carry both keywords. Naming it twice would widen the harvest back to
        // itself for no gain.
        if (region.hold == region.name) {
            region.hold.clear();
        }
        return region;
    }

    std::string Get(const std::string& a_region)
    {
        if (a_region.empty()) {
            return {};
        }

        {
            std::lock_guard lock{ g_mutex };
            if (const auto it = g_cache.find(a_region); it != g_cache.end()) {
                return it->second;
            }
        }

        // Not in memory - a previous session may have left one on disk. That
        // persistence is the point: the cost amortises across sessions too.
        //
        // A MISS is cached as well, deliberately. The panel asks this question
        // every frame it is open, and a region with no digest would otherwise
        // mean a filesystem hit per frame forever. A later Build overwrites the
        // empty entry with the real text, so nothing goes stale that matters.
        auto text = ReadFile(CacheDir() / (Sanitise(a_region) + ".txt"));

        std::lock_guard lock{ g_mutex };
        g_cache[a_region] = text;
        return text;
    }

    bool Building() { return g_building.load(); }

    bool Build(const Region& a_region, bool a_force)
    {
        if (!a_region.Valid()) {
            logs::warn("digest: no region resolved for the current location"sv);
            return false;
        }
        if (g_building.load()) {
            return false;
        }
        if (!a_force && !Get(a_region.name).empty()) {
            return true;   // already have one
        }
        if (!SN::CanGenerate()) {
            logs::error("digest: SkyrimNet cannot generate (API older than v8)"sv);
            return false;
        }

        const auto candidates = GatherCandidates(
            a_region, static_cast<std::size_t>(Config::Get().digestMaxCandidates));

        const std::string context = std::string{ "{" } +
                                    "\"regionName\":\"" + Json::Escape(a_region.name) + "\"," +
                                    "\"holdName\":\"" + Json::Escape(a_region.hold) + "\"," +
                                    "\"knownLocals\":\"" + Json::Escape(candidates) + "\"}";

        g_building.store(true);

        // The completion runs on a SkyrimNet ThreadPool worker: pure file I/O,
        // no RE:: calls, same discipline as the bio generator's.
        const bool queued = SN::SendCustomPrompt(
            kPromptName.data(), kVariant.data(), context.c_str(),
            [region = a_region.name](const char* a_response, int a_success) {
                if (a_response && a_success != 0) {
                    const auto text = KeepBullets(a_response);
                    if (text.empty()) {
                        logs::error("digest: {} came back with no bullet lines - not cached"sv,
                                    region);
                    } else {
                        {
                            std::lock_guard lock{ g_mutex };
                            g_cache[region] = text;
                        }

                        std::error_code ec;
                        std::filesystem::create_directories(CacheDir(), ec);
                        std::ofstream out{ CacheDir() / (Sanitise(region) + ".txt"),
                                           std::ios::binary | std::ios::trunc };
                        out.write(text.data(), static_cast<std::streamsize>(text.size()));

                        logs::info("digest: built for {} ({} chars)"sv, region, text.size());
                    }
                } else {
                    logs::error("digest: build failed for {} - {}"sv, region,
                                a_response ? a_response : "empty response");
                }
                g_building.store(false);
            });

        if (!queued) {
            g_building.store(false);
            logs::error("digest: SkyrimNet did not queue the request"sv);
            return false;
        }

        logs::info("digest: building for {}..."sv, a_region.name);
        return true;
    }
}
