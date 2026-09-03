#include "pch.h"

#include "RegionDigest.h"

#include "Config.h"
#include "Json.h"
#include "SkyrimNetAPI.h"
#include "StagingStore.h"

#include <atomic>
#include <cctype>
#include <chrono>
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

        // Score bands. A bio naming the settlement always outranks one that
        // only names the hold, so hold-only entries fill leftover room and can
        // never crowd out the settlement's own cast.
        //
        // The tier is a LOCALNESS filter; the ranking within it is IMPORTANCE,
        // and those are different questions. Localness was never the problem -
        // measured on a 3,200-bio corpus, the top 120 was already 114 genuine
        // locals and zero outsiders. The problem was that nearly all of them
        // tied, so the cut fell alphabetically: Riften filled up around "V" and
        // dropped the Ragged Flagon's proprietor for having a late initial.
        // Co-citation - how often the rest of the local cast names you - sorts
        // that out, and it is exactly the question the digest prompt asks.
        constexpr int kInSummary     = 1000;   // settlement named in the summary
        constexpr int kInBody        = 100;    // settlement named somewhere else
        constexpr int kHoldInSummary = 20;     // hold-only, named in the summary
        constexpr int kHoldInBody    = 1;      // hold-only, mentioned in passing

        // Phrasing signals. Deliberately SMALL: once a bio is in the summary
        // tier its localness is settled, so these only break ties. Weighted
        // higher they beat real citations, which had the Ragged Flagon's
        // bouncer losing his slot to a passer-by whose summary happened to read
        // "in Riften" rather than "the Riften docks".
        constexpr int kLocative = 40;   // "in/at/of Riften", "Riften's"
        constexpr int kEarly    = 20;   // named in the first third of the summary

        // Penalties DEMOTE, they never exclude. A wrongly dropped local is
        // invisible; a wrongly kept one at least stays inspectable in the sheet.
        constexpr int kParenPenalty      = -600;   // "(actually Svidi from Riften)"
        constexpr int kOtherFirstPenalty = -600;   // another settlement named first
        constexpr int kOriginPenalty     = -150;   // "from Riften", not "from Riften's"

        // Citations dominate phrasing but not the tier, so a well-known local
        // outranks an unknown one without a mod-added NPC ever being buried
        // under vanilla characters the model already knows.
        constexpr int kCiteWeight = 12;
        constexpr int kCiteCap    = 60;

        constexpr double kEarlyFraction = 0.35;

        // For the "names somewhere else first" penalty. Vanilla only: a mod
        // settlement simply does not trigger it, and since the signal only ever
        // demotes, an unlisted place costs accuracy and never correctness.
        constexpr std::string_view kSettlements[] = {
            "Whiterun"sv, "Solitude"sv, "Markarth"sv, "Windhelm"sv, "Dawnstar"sv,
            "Morthal"sv, "Falkreath"sv, "Winterhold"sv, "Riften"sv, "Ivarstead"sv,
            "Rorikstead"sv, "Kynesgrove"sv, "Shor's Stone"sv, "Riverwood"sv,
            "Dragon Bridge"sv, "Karthwasten"sv, "Helgen"sv, "Solstheim"sv,
            "Raven Rock"sv, "Darkwater Crossing"sv, "Old Hroldan"sv
        };

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
            // The captured summary starts with the newline after the block tag.
            while (!a_text.empty() && (a_text.front() == ' ' || a_text.front() == '\n' ||
                                       a_text.front() == '\r' || a_text.front() == '\t')) {
                a_text.remove_prefix(1);
            }

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

        std::size_t CountOccurrences(std::string_view a_haystack, std::string_view a_needle)
        {
            if (a_needle.empty()) {
                return 0;
            }
            std::size_t count = 0;
            for (auto pos = a_haystack.find(a_needle); pos != std::string_view::npos;
                 pos      = a_haystack.find(a_needle, pos + a_needle.size())) {
                ++count;
            }
            return count;
        }

        struct ScoredBio
        {
            int         score{};
            int         cites{};
            std::string name;       // display name, for citation counting
            std::string ties;       // this bio's own relationships block
            std::string line;       // the "- Name: summary" the prompt receives
        };

        // The dozen characters before a mention, lowercased - enough to see an
        // "in ", "at ", "of " or "from " in front of it.
        std::string Preceding(std::string_view a_text, std::size_t a_at)
        {
            const auto start = a_at >= 12 ? a_at - 12 : std::size_t{ 0 };
            std::string out{ a_text.substr(start, a_at - start) };
            for (auto& ch : out) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return out;
        }

        // Ends with a_word (which carries its own trailing space) as a whole
        // word, so "cabin " does not read as "in ".
        bool EndsWithWord(std::string_view a_lower, std::string_view a_word)
        {
            if (!a_lower.ends_with(a_word)) {
                return false;
            }
            if (a_lower.size() == a_word.size()) {
                return true;
            }
            const auto prev = static_cast<unsigned char>(
                a_lower[a_lower.size() - a_word.size() - 1]);
            return std::isalnum(prev) == 0;
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

        // Existing bios that name this place, ranked. The candidate source is
        // the user's OWN installed corpus: each bio's summary block is a
        // ready-made description, and the set reflects their actual load order,
        // mod-added characters included.
        //
        // Tuning lives in tools/rank_probe.py, which runs this same scoring
        // against the real corpus with no game running. Change the weights
        // there first, look at what moves, then mirror them into the constants
        // above.
        std::string GatherCandidates(const Region& a_region, std::size_t a_max)
        {
            const auto      dir = Staging::PromptsDir() / "characters";
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) {
                logs::warn("digest: no bio corpus at {}"sv, dir.string());
                return {};
            }

            const auto started = std::chrono::steady_clock::now();

            std::vector<ScoredBio> scored;
            std::size_t            examined = 0;
            std::size_t            holdOnly = 0;

            for (const auto& file : std::filesystem::directory_iterator{ dir, ec }) {
                if (!file.is_regular_file() || file.path().extension() != ".prompt") {
                    continue;   // also skips the .prompt.backup.<time> files
                }
                ++examined;

                const auto text = ReadFile(file.path());
                if (text.empty()) {
                    continue;
                }

                const auto regionHits = CountOccurrences(text, a_region.name);
                const auto holdHits   = CountOccurrences(text, a_region.hold);
                if (regionHits == 0 && holdHits == 0) {
                    continue;
                }

                const auto summary = Staging::ExtractSummary(text);
                if (summary.empty()) {
                    continue;   // nothing to describe them with
                }

                int        score = 0;
                const auto at    = summary.find(a_region.name);

                if (regionHits > 0) {
                    score += at != std::string::npos ? kInSummary : kInBody;
                } else {
                    ++holdOnly;
                    score += summary.find(a_region.hold) != std::string::npos
                                 ? kHoldInSummary
                                 : kHoldInBody;
                }

                if (at != std::string::npos) {
                    const auto before = Preceding(summary, at);
                    const bool possessive =
                        summary.compare(at, a_region.name.size() + 2,
                                        a_region.name + "'s") == 0;

                    if (possessive || EndsWithWord(before, "in "sv) ||
                        EndsWithWord(before, "at "sv) || EndsWithWord(before, "of "sv) ||
                        EndsWithWord(before, "near "sv)) {
                        score += kLocative;
                    }
                    if (static_cast<double>(at) / static_cast<double>(summary.size()) <
                        kEarlyFraction) {
                        score += kEarly;
                    }

                    // Inside parentheses: almost always somebody ELSE's origin.
                    const auto opened = summary.rfind('(', at);
                    const auto closed = summary.rfind(')', at);
                    if (opened != std::string::npos &&
                        (closed == std::string::npos || opened > closed)) {
                        score += kParenPenalty;
                    }

                    // Another settlement named first: they live there, not here.
                    for (const auto& other : kSettlements) {
                        if (other == a_region.name) {
                            continue;
                        }
                        const auto where = summary.find(other);
                        if (where != std::string::npos && where < at) {
                            score += kOtherFirstPenalty;
                            break;
                        }
                    }

                    // "from Riften" is an origin; "from Riften's docks" is not.
                    if (EndsWithWord(before, "from "sv) && !possessive) {
                        score += kOriginPenalty;
                    }
                }

                ScoredBio bio;
                bio.score = score;
                bio.name  = NameFromStem(file.path().stem().string());
                bio.ties  = Staging::ExtractBlock(text, "relationships"sv);
                bio.line  = "- " + bio.name + ": " + FirstSentence(summary, 240) + "\n";
                scored.push_back(std::move(bio));
            }

            // Co-citation, over the CANDIDATES' relationships blocks rather than
            // the whole corpus. Measured against the full-corpus version the
            // kept set differs by one bio in a hundred and twenty, for a tenth
            // of the text to scan - and locals naming locals is arguably the
            // truer signal anyway.
            std::string ties;
            for (const auto& bio : scored) {
                ties += bio.ties;
                ties += '\n';
            }
            for (auto& bio : scored) {
                // Short single names match too much to count honestly.
                if (bio.name.size() < 5 && bio.name.find(' ') == std::string::npos) {
                    continue;
                }
                const auto total = CountOccurrences(ties, bio.name);
                const auto own   = CountOccurrences(bio.ties, bio.name);
                bio.cites        = static_cast<int>(total > own ? total - own : 0);
                bio.score += kCiteWeight * (std::min)(bio.cites, kCiteCap);
            }

            // Stable: equal scores keep directory order, so the same corpus
            // produces the same sheet twice.
            std::stable_sort(scored.begin(), scored.end(),
                             [](const ScoredBio& a_lhs, const ScoredBio& a_rhs) {
                                 return a_lhs.score > a_rhs.score;
                             });

            // One slot per character. A corpus can hold two files for the same
            // person under different reference FormIDs, and offering the model
            // the same name twice buys nothing.
            std::string out;
            std::size_t kept = 0;
            std::size_t dupes = 0;
            std::vector<std::string> takenNames;
            for (const auto& bio : scored) {
                if (kept >= a_max) {
                    break;
                }
                std::string key = bio.name;
                for (auto& ch : key) {
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                }
                if (std::find(takenNames.begin(), takenNames.end(), key) !=
                    takenNames.end()) {
                    ++dupes;
                    continue;
                }
                takenNames.push_back(std::move(key));
                out += bio.line;
                ++kept;
            }

            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
            logs::info(
                "digest: {} - {} of {} bio(s) match ({} hold-only), keeping the {} strongest"
                " ({} duplicate name(s) skipped) [{}ms]"sv,
                a_region.name, scored.size(), examined, holdOnly, kept, dupes, ms);
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
