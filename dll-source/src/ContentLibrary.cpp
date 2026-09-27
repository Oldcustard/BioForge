#include "pch.h"

#include "ContentLibrary.h"
#include "SkyrimNetAPI.h"

#include <cctype>
#include <fstream>
#include <set>

namespace BioForge::ContentLibrary
{
    namespace
    {
        // Reserved content roots under <SkyrimNet>/, from SkyrimNet's own
        // layer discovery. Anything else on disk there (config/, data/,
        // logs/, the dead Beta 24 prompts/ tree...) is not a content layer
        // and must not be swept.
        constexpr std::string_view kPluginRoots[] = { "library"sv, "external"sv };
        constexpr auto             kOverlayRoot   = "overlay"sv;
        constexpr auto             kSavesRoot     = "saves"sv;

        // The reserved always-on base layer: library/skyrimnet.base, shipped
        // inside the SkyrimNet mod itself. Lower than every other plugin.
        constexpr auto kBaseId = "skyrimnet.base"sv;

        std::string Lower(std::string_view a_s)
        {
            std::string out{ a_s };
            std::transform(out.begin(), out.end(), out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        // Minimal scan of content-registry.json: the set of plugin ids whose
        // "enabled" is false. The file is flat, fixed-shape SkyrimNet output
        // (no JSON library in this plugin on purpose - see Json.h), so
        // hand-scanning it is the same bargain Generator.cpp makes with its
        // two payloads. Empty result when there is no registry yet (nothing
        // installed from the Hub) - then nothing is disabled.
        //
        // Shape walked: "plugins": { "<id>": { "enabled": false, ... }, ... }
        // - one nesting level, ids quoted, no braces inside ids.
        std::set<std::string, std::less<>> DisabledPlugins(const std::filesystem::path& a_registry)
        {
            std::set<std::string, std::less<>> disabled;

            std::ifstream in{ a_registry, std::ios::binary };
            if (!in) {
                return disabled;
            }
            const std::string text{ std::istreambuf_iterator<char>{ in },
                                    std::istreambuf_iterator<char>{} };

            const auto pluginsAt = text.find("\"plugins\"");
            if (pluginsAt == std::string::npos) {
                return disabled;
            }

            // Walk member-by-member of the plugins object: a quoted key at
            // depth 1, then its object value, testing it for "enabled": false.
            std::size_t pos    = text.find('{', pluginsAt);
            int         depth  = 0;
            std::string currentId;
            bool        haveId = false;
            while (pos < text.size()) {
                const char ch = text[pos];
                if (ch == '{') {
                    ++depth;
                } else if (ch == '}') {
                    --depth;
                    if (depth == 0) {
                        break;   // end of the plugins object
                    }
                } else if (ch == '"') {
                    const auto end = text.find('"', pos + 1);
                    if (end == std::string::npos) {
                        break;
                    }
                    if (depth == 1 && !haveId) {
                        currentId = text.substr(pos + 1, end - pos - 1);
                        haveId    = true;
                    }
                    pos = end;
                } else if (ch == ':' && depth == 1 && haveId) {
                    // The member value: one nested object. Find where it
                    // ends (brace-matched) and test that span.
                    const auto open = text.find('{', pos);
                    if (open == std::string::npos) {
                        break;
                    }
                    int         vDepth = 0;
                    std::size_t vEnd    = open;
                    for (; vEnd < text.size(); ++vEnd) {
                        if (text[vEnd] == '{') {
                            ++vDepth;
                        } else if (text[vEnd] == '}') {
                            if (--vDepth == 0) {
                                break;
                            }
                        }
                    }
                    // "enabled" itself must be false - a sibling such as
                    // "enabled_explicit": false says nothing about it.
                    const auto value = text.substr(open, vEnd - open + 1);
                    if (const auto key = value.find("\"enabled\""); key != std::string::npos) {
                        auto at = value.find_first_not_of(" \t\r\n", key + 9);
                        if (at != std::string::npos && value[at] == ':') {
                            at = value.find_first_not_of(" \t\r\n", at + 1);
                            if (at != std::string::npos && value.compare(at, 5, "false") == 0) {
                                disabled.insert(currentId);
                            }
                        }
                    }
                    currentId.clear();
                    haveId = false;
                    pos    = vEnd;
                }
                ++pos;
            }
            return disabled;
        }

        // "gudra_59A6.prompt" -> "gudra_59A6"; "gudra_59A6.dynamic.prompt"
        // -> "gudra_59A6" (the same bio slot - dynamic is how it was written,
        // not a different character). Anything else is not a bio.
        bool StemOf(const std::wstring& a_file, std::string& a_out, bool& a_dynamic)
        {
            constexpr auto kPrompt = L".prompt"sv;
            constexpr auto kDyn    = L".dynamic"sv;

            if (a_file.size() <= kPrompt.size() + 1 ||
                a_file.compare(a_file.size() - kPrompt.size(), kPrompt.size(), kPrompt) != 0) {
                return false;
            }
            auto stem = a_file.substr(0, a_file.size() - kPrompt.size());

            a_dynamic = false;
            if (stem.size() > kDyn.size() + 1 &&
                stem.compare(stem.size() - kDyn.size(), kDyn.size(), kDyn) == 0) {
                stem.resize(stem.size() - kDyn.size());
                a_dynamic = true;
            }
            if (stem.empty()) {
                return false;
            }
            a_out.clear();
            a_out.reserve(stem.size());
            for (const wchar_t ch : stem) {
                a_out += static_cast<char>(ch);   // ASCII by the naming rule
            }
            return !a_out.empty();
        }
    }

    bool StemLess::operator()(std::string_view a_lhs, std::string_view a_rhs) const
    {
        return std::lexicographical_compare(
            a_lhs.begin(), a_lhs.end(), a_rhs.begin(), a_rhs.end(),
            [](unsigned char a, unsigned char b) { return std::tolower(a) < std::tolower(b); });
    }

    void Index::Offer(const std::string& a_stem, const BioFile& a_file)
    {
        // SkyrimNet's resolution order, highest first: per-save, overlay,
        // plugins (base lowest), and - within a tier - a deliberately written
        // static bio over the engine's dynamic draft of the same character.
        auto Rank = [](const BioFile& b) {
            int rank = 0;
            switch (b.source) {
            case Source::PerSave: rank = 30; break;
            case Source::Overlay: rank = 20; break;
            case Source::Plugin:  rank = b.isBase ? 1 : 10; break;
            case Source::None:    return 0;
            }
            return rank - (b.dynamic ? 1 : 0);
        };

        auto [it, inserted] = m_bios.try_emplace(a_stem, a_file);
        if (!inserted && Rank(a_file) > Rank(it->second)) {
            it->second = a_file;
        }
    }

    void Index::SweepLayer(const std::filesystem::path& a_layerDir, Source a_source,
                           bool a_isBase)
    {
        std::error_code ec;
        const auto      characters = a_layerDir / "prompts" / "characters";
        if (!std::filesystem::is_directory(characters, ec)) {
            return;
        }
        ++m_layersSeen;

        // recursive covers the reserved dynamic/ subfolder along with the
        // flat bios; StemOf separates the two spellings of one slot.
        for (const auto& entry : std::filesystem::recursive_directory_iterator{
                 characters, std::filesystem::directory_options::skip_permission_denied, ec }) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            std::string stem;
            bool        dynamic = false;
            if (!StemOf(entry.path().filename().wstring(), stem, dynamic)) {
                continue;   // .backup.<unixtime> siblings and friends
            }
            Offer(stem, BioFile{ entry.path(), a_source, dynamic, a_isBase });
        }
    }

    std::filesystem::path SkyrimNetDir()
    {
        // Resolve from this DLL's own module: the game process's USVFS view
        // of Data/SKSE/Plugins is what SkyrimNet reads and writes through,
        // which is the whole point of going through it from in here.
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&SkyrimNetDir), &module);

        wchar_t buffer[MAX_PATH * 2]{};
        if (!module || !GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)))) {
            logs::error("content: cannot locate own module - falling back to relative path"sv);
            return std::filesystem::path{ "Data/SKSE/Plugins/SkyrimNet" };
        }

        // <...>/SKSE/Plugins/BioForge.dll -> <...>/SKSE/Plugins/SkyrimNet
        return std::filesystem::path{ buffer }.parent_path() / "SkyrimNet";
    }

    void Index::Build()
    {
        m_bios.clear();
        m_layersSeen = 0;

        const auto root = SkyrimNetDir();
        {
            std::error_code ec;
            if (!std::filesystem::is_directory(root, ec)) {
                logs::warn("content: no SkyrimNet tree at {}"sv, root.string());
                return;
            }
        }

        const auto disabled = DisabledPlugins(root / "content-registry.json");

        // Plugin layers live one id deep under library/ and external/. Hub
        // installs land in the overwrite folder's library/, the shipped base
        // inside the SkyrimNet mod's own library/ - the VFS shows both as one
        // tree. A plugin disabled in the registry is skipped whole: its files
        // are still on disk but do not resolve.
        for (const auto rootName : kPluginRoots) {
            std::error_code ec;
            for (const auto& id : std::filesystem::directory_iterator{
                     root / rootName, std::filesystem::directory_options::skip_permission_denied,
                     ec }) {
                if (!id.is_directory(ec)) {
                    continue;
                }
                const auto idName = Lower(id.path().filename().string());
                if (idName != kBaseId && disabled.contains(idName)) {
                    continue;
                }
                SweepLayer(id.path(), Source::Plugin, idName == kBaseId);
            }
        }

        // The player's own layer: where dashboard edits - and Bio Forge
        // commits - land, at overlay/prompts/characters directly.
        SweepLayer(root / kOverlayRoot, Source::Overlay, false);

        // The per-save layer: saves/<saveId>/prompts/characters, holding this
        // playthrough's bios and the engine's dynamic drafts. These outrank
        // everything static at RENDER time in SkyrimNet, which is why they
        // are indexed rather than treated as absent. Only the LOADED save's
        // folder: another playthrough's bio does not cover anyone here. No
        // save loaded means no per-save layer, as in SkyrimNet itself.
        if (const auto saveId = SN::SaveUniqueID(); !saveId.empty()) {
            SweepLayer(root / kSavesRoot / saveId, Source::PerSave, false);
        }
    }

    const BioFile* Index::Find(std::string_view a_stem) const
    {
        const auto it = m_bios.find(a_stem);
        return it == m_bios.end() ? nullptr : &it->second;
    }

    std::string ReadBioFile(const std::filesystem::path& a_path)
    {
        std::ifstream in{ a_path, std::ios::binary };
        if (!in) {
            return {};
        }
        return std::string{ std::istreambuf_iterator<char>{ in },
                            std::istreambuf_iterator<char>{} };
    }
}
