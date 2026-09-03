#include "pch.h"

#include "HoldDigest.h"

#include "SkyrimNetAPI.h"
#include "StagingStore.h"

#include <atomic>
#include <fstream>
#include <mutex>
#include <regex>

namespace BioForge::HoldDigest
{
    namespace
    {
        constexpr auto kPromptName = "bioforge_hold_digest"sv;
        constexpr auto kVariant    = "CharacterProfileGeneration"sv;

        // How many existing bios to offer as candidates. The model needs enough
        // of the local cast to choose from, but this is the one genuinely large
        // block in the prompt, so it is capped.
        constexpr std::size_t kMaxCandidates = 120;

        std::mutex                                   g_mutex;
        std::map<std::string, std::string, std::less<>> g_cache;
        std::atomic<bool>                            g_building{ false };

        std::filesystem::path CacheDir()
        {
            // Beside the DLL, NOT under prompts/: SkyrimNet scans that tree and
            // these are data files, not templates.
            return Staging::PromptsDir().parent_path().parent_path() / "BioForge" / "holds";
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
        // display name lowercased with spaces as underscores, plus a ref suffix.
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
                out += capitalise ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                                  : c;
                capitalise = (c == ' ' || c == '-');
            }
            return out;
        }

        std::string FirstSentence(std::string_view a_text, std::size_t a_maxChars)
        {
            const auto stop = a_text.find(". ");
            auto       cut  = stop == std::string_view::npos ? a_text.size() : stop + 1;
            cut             = std::min(cut, a_maxChars);

            std::string out{ a_text.substr(0, cut) };
            for (auto& ch : out) {
                if (ch == '\n' || ch == '\r') {
                    ch = ' ';
                }
            }
            return out;
        }

        // Existing bios that mention this hold or its city. The user's own bio
        // corpus is the description source: it says who these people are, and
        // it already reflects whatever mods they actually run.
        std::string GatherCandidates(const std::string& a_hold)
        {
            // "The Rift" -> also match "Riften". Hold and city names differ, and
            // bios overwhelmingly name the city.
            std::vector<std::string> needles{ a_hold };
            {
                static const std::map<std::string, std::string, std::less<>> cities{
                    { "The Rift", "Riften" },       { "Whiterun Hold", "Whiterun" },
                    { "Eastmarch", "Windhelm" },    { "The Reach", "Markarth" },
                    { "Haafingar", "Solitude" },    { "Hjaalmarch", "Morthal" },
                    { "The Pale", "Dawnstar" },     { "Winterhold", "Winterhold" },
                    { "Falkreath Hold", "Falkreath" }
                };
                if (const auto it = cities.find(a_hold); it != cities.end()) {
                    needles.push_back(it->second);
                }
            }

            const auto      dir = Staging::PromptsDir() / "characters";
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) {
                return {};
            }

            static const std::regex summaryRe{ R"(\{%\s*block\s+summary\s*%\}([\s\S]*?)\{%\s*endblock)" };

            std::string out;
            std::size_t found = 0;
            for (const auto& file : std::filesystem::directory_iterator{ dir, ec }) {
                if (found >= kMaxCandidates) {
                    break;
                }
                if (!file.is_regular_file() || file.path().extension() != ".prompt") {
                    continue;
                }

                const auto text = ReadFile(file.path());
                if (text.empty()) {
                    continue;
                }

                const bool relevant = std::any_of(needles.begin(), needles.end(),
                                                  [&](const std::string& n) {
                                                      return text.find(n) != std::string::npos;
                                                  });
                if (!relevant) {
                    continue;
                }

                std::smatch m;
                if (!std::regex_search(text, m, summaryRe)) {
                    continue;
                }

                out += "- " + NameFromStem(file.path().stem().string()) + ": " +
                       FirstSentence(m[1].str(), 240) + "\n";
                ++found;
            }

            logs::info("digest: {} candidate bio(s) mention {}"sv, found, a_hold);
            return out;
        }

        std::string JsonEscape(std::string_view a_s)
        {
            std::string out;
            out.reserve(a_s.size() + 8);
            for (const char ch : a_s) {
                switch (ch) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (static_cast<unsigned char>(ch) < 0x20) {
                        char buf[7];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                        out += buf;
                    } else {
                        out += ch;
                    }
                }
            }
            return out;
        }
    }

    std::string CurrentHold()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return {};
        }

        auto* location = player->GetCurrentLocation();
        if (!location) {
            return {};
        }

        // Walk to the top of the location tree: interior -> city -> hold.
        // Bounded in case a mod ever makes the chain circular.
        const auto* best = location;
        for (int hop = 0; hop < 8 && location->parentLoc; ++hop) {
            location = location->parentLoc;
            best     = location;
        }

        const auto* name = best->GetFullName();
        return name ? std::string{ name } : std::string{};
    }

    std::string Get(const std::string& a_hold)
    {
        if (a_hold.empty()) {
            return {};
        }

        {
            std::lock_guard lock{ g_mutex };
            if (const auto it = g_cache.find(a_hold); it != g_cache.end()) {
                return it->second;
            }
        }

        // Not in memory - a previous session may have left one on disk.
        const auto text = ReadFile(CacheDir() / (Sanitise(a_hold) + ".txt"));
        if (!text.empty()) {
            std::lock_guard lock{ g_mutex };
            g_cache[a_hold] = text;
        }
        return text;
    }

    bool Building() { return g_building.load(); }

    bool Build(const std::string& a_hold, bool a_force)
    {
        if (a_hold.empty()) {
            logs::warn("digest: no hold resolved for the current location"sv);
            return false;
        }
        if (g_building.load()) {
            return false;
        }
        if (!a_force && !Get(a_hold).empty()) {
            return true;   // already have one
        }
        if (!SN::CanGenerate()) {
            logs::error("digest: SkyrimNet cannot generate (API older than v8)"sv);
            return false;
        }

        const auto candidates = GatherCandidates(a_hold);

        const std::string context = std::string{ "{" } + "\"holdName\":\"" +
                                    JsonEscape(a_hold) + "\"," + "\"knownLocals\":\"" +
                                    JsonEscape(candidates) + "\"}";

        g_building.store(true);
        const bool queued = SN::SendCustomPrompt(
            kPromptName.data(), kVariant.data(), context.c_str(),
            [hold = a_hold](const char* a_response, int a_success) {
                if (a_response && a_success != 0) {
                    std::string text{ a_response };

                    {
                        std::lock_guard lock{ g_mutex };
                        g_cache[hold] = text;
                    }

                    std::error_code ec;
                    std::filesystem::create_directories(CacheDir(), ec);
                    std::ofstream out{ CacheDir() / (Sanitise(hold) + ".txt"), std::ios::binary };
                    out.write(text.data(), static_cast<std::streamsize>(text.size()));

                    logs::info("digest: built for {} ({} chars)"sv, hold, text.size());
                } else {
                    logs::error("digest: build failed for {} - {}"sv, hold,
                                a_response ? a_response : "empty response");
                }
                g_building.store(false);
            });

        if (!queued) {
            g_building.store(false);
            logs::error("digest: SkyrimNet did not queue the request"sv);
            return false;
        }

        logs::info("digest: building for {}..."sv, a_hold);
        return true;
    }
}
