#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace BioForge::ContentLibrary
{
    // Where a winning bio comes from, in SkyrimNet's resolution order:
    // per-playthrough (per-save) files, the player's overlay edits,
    // installed and external plugins, then skyrimnet.base.
    enum class Source
    {
        None,
        Plugin,   // library/<id>/ or external/<id>/, incl. skyrimnet.base
        Overlay,  // overlay/ - the player's own layer, where commits land
        PerSave   // saves/<id>/ - this playthrough, incl. engine dynamic bios
    };

    struct BioFile
    {
        std::filesystem::path path;
        Source                source{ Source::None };
        bool                  dynamic{};   // engine-generated .dynamic.prompt
        bool                  isBase{};    // the shipped skyrimnet.base layer
    };

    // Stems are UTF-8 and compare case-insensitively. Files on disk mix
    // spellings - albret_F18 beside battlemage_ancel_vaugier_c21 - and the
    // Beta 24 check was a Windows stat, which never cared; a case-sensitive
    // lookup turns a covered NPC into a false gap. Non-ASCII stems (Cyrillic
    // names on a Russian install) fold through Windows' own lowercasing, the
    // same rule the filesystem applies.
    struct StemLess
    {
        using is_transparent = void;
        bool operator()(std::string_view a_lhs, std::string_view a_rhs) const;
    };

    // One pass over the content library. BioForge used to stat one directory
    // (prompts/characters) - Beta 25 spreads the same files across layer
    // folders, so every "does this NPC have a bio" question now goes through
    // an Index built once per scan / harvest / roster build, never per row.
    class Index
    {
    public:
        // Sweep every layer the load order provides. Safe to call any time;
        // cheap enough for a button press, too dear for a per-frame path.
        void Build();

        // The winning file for a bio template stem ("gudra_59A6"), matched
        // case-insensitively; nullptr when no layer has one.
        [[nodiscard]] const BioFile* Find(std::string_view a_stem) const;

        // Every distinct bio, winner only - one entry per character. This is
        // the digest harvest's corpus and the panel's census.
        [[nodiscard]] const std::map<std::string, BioFile, StemLess>& All() const
        {
            return m_bios;
        }

        [[nodiscard]] std::size_t LayersSeen() const { return m_layersSeen; }

    private:
        // Offer one found file as the stem's winner if it outranks what is
        // already indexed (per-save > overlay > plugin, base lowest, static
        // over dynamic within a tier).
        void Offer(const std::string& a_stem, const BioFile& a_file);

        // Sweep <layerDir>/prompts/characters (recursively, so the reserved
        // dynamic/ subfolder is covered) into the index. No-op when the layer
        // has no bios at all.
        void SweepLayer(const std::filesystem::path& a_layerDir, Source a_source,
                        bool a_isBase);

        std::map<std::string, BioFile, StemLess> m_bios;
        std::size_t                              m_layersSeen{};
    };

    // <Data>/SKSE/Plugins/SkyrimNet, resolved from this DLL's own module the
    // same way Staging's paths always were: inside the game process every
    // enumeration below goes through the USVFS view, so hub installs in the
    // overwrite folder and external layers inside mod folders all appear as
    // one merged tree - which is exactly what SkyrimNet itself reads.
    std::filesystem::path SkyrimNetDir();

    // Stems and names are carried as UTF-8 std::string. A std::filesystem::path
    // built straight from a std::string decodes it with the ANSI codepage,
    // which garbles any non-ASCII name - so every stem-to-path hop goes
    // through these. Input that is not valid UTF-8 (a legacy-codepage string)
    // falls back to the ANSI decode rather than throwing.
    std::filesystem::path PathFromUtf8(std::string_view a_utf8);
    std::string           Utf8Of(const std::filesystem::path& a_path);
    bool                  IsValidUtf8(std::string_view a_s);
    std::wstring          WideFromUtf8(std::string_view a_utf8);
    std::string           Utf8FromWide(std::wstring_view a_wide);

    // One bio file's whole text, or "". Used by the roster and the digest
    // harvest; the staging bundle is checked by the caller first.
    std::string ReadBioFile(const std::filesystem::path& a_path);
}
