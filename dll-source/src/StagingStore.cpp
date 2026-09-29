#include "pch.h"

#include "StagingStore.h"

#include "Config.h"
#include "ContentLibrary.h"
#include "ScopeSelector.h"
#include "SkyrimNetAPI.h"
#include "SkyrimNetWeb.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>

namespace BioForge::Staging
{
    namespace
    {
        std::mutex             g_mutex;
        std::vector<Entry>     g_entries;

        std::string_view Trim(std::string_view a_s)
        {
            while (!a_s.empty() && (a_s.front() == ' ' || a_s.front() == '\t' || a_s.front() == '\r')) {
                a_s.remove_prefix(1);
            }
            while (!a_s.empty() &&
                   (a_s.back() == ' ' || a_s.back() == '\t' || a_s.back() == '\r' || a_s.back() == '\n')) {
                a_s.remove_suffix(1);
            }
            return a_s;
        }

        // "### interject summary:" / "#### **Speech Style**" -> block index.
        // Compares letters/digits only, ignoring '_', punctuation and case, so
        // the common model typos still land on the right block.
        int MatchBlockName(std::string_view a_heading)
        {
            std::string clean;
            clean.reserve(a_heading.size());
            for (const char ch : a_heading) {
                if (std::isalnum(static_cast<unsigned char>(ch))) {
                    clean += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                }
            }

            for (std::size_t i = 0; i < std::size(kBlockNames); ++i) {
                std::string want{ kBlockNames[i] };
                want.erase(std::remove(want.begin(), want.end(), '_'), want.end());
                if (clean == want) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        void WriteFile(const std::filesystem::path& a_path, std::string_view a_content)
        {
            std::error_code ec;
            std::filesystem::create_directories(a_path.parent_path(), ec);

            // binary + LF-only: the corpus .prompt files are LF, and text mode
            // would translate every '\n' into CRLF.
            std::ofstream out{ a_path, std::ios::binary | std::ios::trunc };
            out.write(a_content.data(), static_cast<std::streamsize>(a_content.size()));
        }

        std::filesystem::path BundleDir(const Entry& a_entry)
        {
            std::string stem{ a_entry.fileName };
            if (stem.ends_with(".prompt")) {
                stem.resize(stem.size() - 7);
            }
            return StagingRoot() / ContentLibrary::PathFromUtf8(stem);
        }

        // Entry::stagingDir is carried as UTF-8, like every stem it is built from.
        std::filesystem::path StagingDirOf(std::string_view a_stagingDir)
        {
            return ContentLibrary::PathFromUtf8(a_stagingDir);
        }

        Entry* FindLocked(std::uint32_t a_refFormID)
        {
            for (auto& e : g_entries) {
                if (e.refFormID == a_refFormID) {
                    return &e;
                }
            }
            return nullptr;
        }
    }

    std::filesystem::path StagingRoot()
    {
        // Beside the DLL, NOT inside SkyrimNet's content tree: staging
        // bundles are audit trail, not content, and Beta 25's layer scanner
        // rejects anything but real content under its roots.
        return ContentLibrary::SkyrimNetDir().parent_path() / "BioForge" / "staging";
    }

    std::string BioFileName(const Candidate& a_candidate)
    {
        // SkyrimNet's own resolution wins when it has one - it may carry
        // disambiguation the plain convention lacks. It returns the template
        // NAME (no extension), same field ScopeSelector checks existence with.
        std::string resolved{ Trim(a_candidate.bioTemplate) };
        if (resolved.ends_with(".prompt")) {
            resolved.resize(resolved.size() - 7);
        }
        if (!resolved.empty()) {
            return resolved;
        }

        // Non-ASCII bytes are kept when the name is valid UTF-8, so a Cyrillic
        // name keeps its letters instead of collapsing to "npc". A name in a
        // legacy codepage cannot be carried safely and is stripped as before.
        const bool     keepHigh = ContentLibrary::IsValidUtf8(a_candidate.name);
        std::string derived;
        for (const char ch : a_candidate.name) {
            const auto c = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (c == ' ') {
                derived += '_';
            } else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ||
                       (keepHigh && static_cast<unsigned char>(c) >= 0x80)) {
                derived += c;
            }
        }
        if (derived.empty()) {
            derived = "npc";
        }

        char suffix[8]{};
        std::snprintf(suffix, sizeof(suffix), "_%03X", a_candidate.refFormID & 0xFFF);
        return derived + suffix;
    }

    bool ParseResponse(std::string_view a_raw, std::string& a_bioText,
                       std::vector<std::string>& a_missing)
    {
        std::array<std::string, std::size(kBlockNames)> blocks{};

        int  current = -1;   // nothing captured before the first ### heading
        bool inFence = false;

        std::size_t pos = 0;
        while (pos <= a_raw.size()) {
            const auto end = a_raw.find('\n', pos);
            auto line = a_raw.substr(pos, end == std::string_view::npos
                                              ? std::string_view::npos
                                              : end - pos);
            pos = end == std::string_view::npos ? a_raw.size() + 1 : end + 1;

            const auto trimmed = Trim(line);
            if (trimmed.empty()) {
                if (current >= 0 && !blocks[current].empty()) {
                    blocks[current] += '\n';   // paragraph separator inside a block
                }
                continue;
            }
            if (trimmed.starts_with("```")) {
                inFence = !inFence;   // strip fences rather than pollute blocks
                continue;
            }

            const bool isHeading =
                !trimmed.empty() && trimmed.front() == '#' && !inFence;
            if (isHeading) {
                const auto rest  = Trim(trimmed.substr(trimmed.find_first_not_of('#')));
                const auto index = MatchBlockName(rest);
                if (index >= 0) {
                    current = index;
                    continue;
                }
                continue;   // an invented heading line is dropped, not captured
            }

            if (current >= 0) {
                blocks[current] += line;
                blocks[current] += '\n';
            }
            // Preamble before the first recognized heading is discarded.
        }

        a_bioText.clear();
        a_missing.clear();
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            auto content = Trim(blocks[i]);
            if (content.empty()) {
                a_missing.emplace_back(kBlockNames[i]);
                continue;
            }

            a_bioText += "{% block ";
            a_bioText += kBlockNames[i];
            a_bioText += " %}";
            if (content.find('\n') != std::string_view::npos) {
                a_bioText += '\n';
                a_bioText += content;
                a_bioText += '\n';
            } else {
                a_bioText += content;
            }
            a_bioText += "{% endblock %}";
            if (i + 1 < blocks.size()) {
                a_bioText += "\n\n";
            }
        }

        return a_missing.empty();
    }

    void Begin(std::uint32_t a_refFormID, std::string_view a_name, std::string_view a_fileName)
    {
        std::lock_guard lock{ g_mutex };
        // A re-generation replaces any earlier entry for the same reference.
        g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(),
                                       [&](const Entry& e) { return e.refFormID == a_refFormID; }),
                        g_entries.end());
        Entry e{};
        e.refFormID  = a_refFormID;
        e.name       = a_name;
        e.fileName   = a_fileName;
        e.state      = State::Generating;
        g_entries.push_back(std::move(e));
        logs::info("generate: queued {} as {}"sv, a_name, a_fileName);
    }

    void RecordGenerated(std::uint32_t a_refFormID, std::string_view a_contextJson,
                         std::string_view a_rawResponse, const std::string& a_bioText,
                         const std::vector<std::string>& a_missing)
    {
        std::filesystem::path bundle;
        {
            std::lock_guard lock{ g_mutex };
            Entry* e = FindLocked(a_refFormID);
            if (!e) {
                return;
            }
            bundle = BundleDir(*e);

            if (a_bioText.empty()) {
                e->state = State::Failed;
                e->note  = "response missing block(s): ";
                for (std::size_t i = 0; i < a_missing.size(); ++i) {
                    e->note += (i ? ", " : "");
                    e->note += a_missing[i];
                }
            } else {
                e->state = State::Staged;
                e->note.clear();
            }
            e->stagingDir = ContentLibrary::Utf8Of(bundle);
        }

        // The bundle is the audit trail: what the model was asked (harvest),
        // what it answered (raw), and what BioForge would commit (bio).
        WriteFile(bundle / "harvest.json", a_contextJson);
        WriteFile(bundle / "response.raw.txt", a_rawResponse);

        if (!a_bioText.empty()) {
            WriteFile(bundle / "bio.prompt", a_bioText);
        }

        std::lock_guard lock{ g_mutex };
        if (Entry* e = FindLocked(a_refFormID)) {
            logs::info("generate: {} {} (bundle: {})"sv, e->name,
                       e->state == State::Staged ? "staged"sv : "failed parse"sv,
                       ContentLibrary::Utf8Of(bundle));
        }
    }

    void RecordFailed(std::uint32_t a_refFormID, std::string_view a_reason)
    {
        std::lock_guard lock{ g_mutex };
        Entry* e = FindLocked(a_refFormID);
        if (!e) {
            return;
        }
        e->state = State::Failed;
        e->note  = a_reason;
        logs::error("generate: {} failed - {}"sv, e->name, a_reason);
    }

    std::vector<Entry> Snapshot()
    {
        std::lock_guard lock{ g_mutex };
        return g_entries;
    }

    namespace
    {
        std::string ReadWholeFile(const std::filesystem::path& a_path)
        {
            std::ifstream in{ a_path, std::ios::binary };
            if (!in) {
                return {};
            }
            return std::string{ std::istreambuf_iterator<char>{ in },
                                std::istreambuf_iterator<char>{} };
        }
    }

    namespace
    {
        // An unfilled corpus template: "Feris is a [DESCRIPTION]." Rare - two
        // files in a 3,200-bio corpus - but handing one to a neighbour's
        // generation is worse than handing over nothing, because the roster
        // presents these lines as authoritative fact. Treat it as unwritten.
        bool IsPlaceholder(std::string_view a_summary)
        {
            for (std::size_t i = 0; i + 1 < a_summary.size(); ++i) {
                if (a_summary[i] != '[') {
                    continue;
                }
                const auto close = a_summary.find(']', i + 1);
                if (close == std::string_view::npos) {
                    return false;
                }
                const auto inner = a_summary.substr(i + 1, close - i - 1);
                const bool shouty =
                    inner.size() >= 3 &&
                    std::all_of(inner.begin(), inner.end(), [](unsigned char ch) {
                        return std::isupper(ch) || ch == '_' || ch == ' ';
                    });
                if (shouty) {
                    return true;
                }
            }
            return false;
        }
    }

    std::string ExtractBlock(std::string_view a_promptText, std::string_view a_blockName)
    {
        // Hand-scanned rather than regexed: the block markers are fixed text.
        const std::string open  = "block " + std::string{ a_blockName } + " %}";
        constexpr auto    close = "{% endblock"sv;

        const auto at = a_promptText.find(open);
        if (at == std::string_view::npos) {
            return {};
        }
        const auto bodyStart = at + open.size();
        const auto bodyEnd   = a_promptText.find(close, bodyStart);
        if (bodyEnd == std::string_view::npos) {
            return {};
        }
        return std::string{ Trim(a_promptText.substr(bodyStart, bodyEnd - bodyStart)) };
    }

    std::string ExtractSummary(std::string_view a_promptText)
    {
        return ExtractBlock(a_promptText, "summary"sv);
    }

    std::string ExtractSection(std::string_view a_raw, std::string_view a_sectionName)
    {
        std::string body;

        int         current = -1;   // -1 until the wanted heading is seen
        std::size_t pos     = 0;
        while (pos <= a_raw.size()) {
            const auto end  = a_raw.find('\n', pos);
            const auto line = a_raw.substr(pos, end == std::string_view::npos
                                                   ? std::string_view::npos
                                                   : end - pos);
            pos = end == std::string_view::npos ? a_raw.size() + 1 : end + 1;

            const auto trimmed = Trim(line);
            if (!trimmed.empty() && trimmed.front() == '#') {
                const auto hashEnd = trimmed.find_first_not_of('#');
                if (hashEnd == std::string_view::npos) {
                    continue;   // a bare "###" line, not a heading
                }
                const auto name = Trim(trimmed.substr(hashEnd));
                if (MatchBlockName(name) == MatchBlockName(a_sectionName) &&
                    MatchBlockName(name) >= 0) {
                    current = 1;
                    continue;
                }
                if (current > 0) {
                    break;   // next heading ends the section we wanted
                }
                continue;
            }

            if (current > 0) {
                body += line;
                body += '\n';
            }
        }

        return std::string{ Trim(body) };
    }

    std::string BioSummary(const Candidate& a_candidate,
                           const ContentLibrary::Index& a_committed)
    {
        std::string file{ BioFileName(a_candidate) };
        if (file.ends_with(".prompt")) {
            file.resize(file.size() - 7);
        }

        // Staging first: a batch-mate written minutes ago has not been
        // committed yet, but it is the truest thing available about them.
        // Then the content library's winning copy, wherever it lives.
        std::vector<std::filesystem::path> candidates{ StagingRoot() /
                                                       ContentLibrary::PathFromUtf8(file) /
                                                       "bio.prompt" };
        if (const auto* winning = a_committed.Find(file)) {
            candidates.push_back(winning->path);
        }

        for (const auto& path : candidates) {
            const auto text = ReadWholeFile(path);
            if (text.empty()) {
                continue;
            }
            auto summary = ExtractSummary(text);
            if (!summary.empty() && !IsPlaceholder(summary)) {
                // One line: the roster is injected into every job in the batch,
                // so a full summary each would crowd out the NPC's own evidence.
                for (auto& ch : summary) {
                    if (ch == '\n' || ch == '\r') {
                        ch = ' ';
                    }
                }
                // TWO sentences, not one. A first sentence is very often pure
                // scene-setting - "Haknir is a Nord man presently at Mara's
                // Embrace, the brothel beside Honeyside" places him and says
                // nothing about who he is - and a neighbour handed only that
                // can say nothing useful in return. The role usually lands in
                // the second. Compare the sibling written the same minute:
                // "Phoenia is a Breton courtesan working at Mara's Embrace"
                // carries her trade in sentence one, and her ties came out
                // specific where his came out hedged.
                //
                // Still capped: this rides along on every job in the batch.
                std::size_t cut       = std::string::npos;
                std::size_t searchPos = 0;
                for (int sentence = 0; sentence < 2; ++sentence) {
                    const auto stop = summary.find(". ", searchPos);
                    if (stop == std::string::npos) {
                        break;   // no more sentence breaks; keep what we have
                    }
                    cut       = stop + 1;
                    searchPos = stop + 2;
                    if (cut >= 200) {
                        break;   // already long enough to be worth something
                    }
                }
                if (cut != std::string::npos) {
                    summary.resize(cut);
                }
                if (summary.size() > 280) {
                    summary.resize(280);
                }
                return summary;
            }
        }
        return {};
    }

    std::string ReadStagedBio(const Entry& a_entry)
    {
        if (a_entry.stagingDir.empty()) {
            return {};
        }
        return ReadWholeFile(StagingDirOf(a_entry.stagingDir) / "bio.prompt");
    }

    std::string ReadRawResponse(const Entry& a_entry)
    {
        if (a_entry.stagingDir.empty()) {
            return {};
        }
        return ReadWholeFile(StagingDirOf(a_entry.stagingDir) / "response.raw.txt");
    }

    std::string StagedBioFor(std::uint32_t a_refFormID)
    {
        std::filesystem::path dir;
        {
            std::lock_guard lock{ g_mutex };
            const Entry*    e = FindLocked(a_refFormID);
            if (!e || e->stagingDir.empty()) {
                return {};
            }
            dir = StagingDirOf(e->stagingDir);
        }
        return ReadWholeFile(dir / "bio.prompt");
    }

    bool ApplyRefinedBlock(std::uint32_t a_refFormID, std::string_view a_blockName,
                           std::string_view a_rawResponse, std::string& a_note)
    {
        std::filesystem::path dir;
        {
            std::lock_guard lock{ g_mutex };
            const Entry*    e = FindLocked(a_refFormID);
            if (!e || e->stagingDir.empty()) {
                a_note = "nothing staged to refine";
                return false;
            }
            dir = StagingDirOf(e->stagingDir);
        }

        // Keep the reply either way - a refine that made things worse is only
        // reviewable if the raw text survives.
        WriteFile(dir / "refine.raw.txt", a_rawResponse);

        const auto replacement = ExtractSection(a_rawResponse, a_blockName);
        if (replacement.empty()) {
            a_note = "refine reply had no " + std::string{ a_blockName } + " section";
            return false;
        }

        const auto bio = ReadWholeFile(dir / "bio.prompt");
        if (bio.empty()) {
            a_note = "staged bio.prompt could not be read";
            return false;
        }

        const std::string open  = "{% block " + std::string{ a_blockName } + " %}";
        constexpr auto    close = "{% endblock %}"sv;

        const auto at = bio.find(open);
        if (at == std::string::npos) {
            a_note = "staged bio has no " + std::string{ a_blockName } + " block";
            return false;
        }
        const auto bodyEnd = bio.find(close, at + open.size());
        if (bodyEnd == std::string::npos) {
            a_note = "staged " + std::string{ a_blockName } + " block is unterminated";
            return false;
        }

        // Match how ParseResponse lays a block out: multi-line bodies sit on
        // their own lines, single-line bodies stay inline.
        std::string rebuilt{ bio.substr(0, at + open.size()) };
        if (replacement.find('\n') != std::string::npos) {
            rebuilt += '\n';
            rebuilt += replacement;
            rebuilt += '\n';
        } else {
            rebuilt += replacement;
        }
        rebuilt += bio.substr(bodyEnd);

        WriteFile(dir / "bio.prompt", rebuilt);

        a_note = "ties rewritten against the finished roster";
        {
            std::lock_guard lock{ g_mutex };
            if (Entry* e = FindLocked(a_refFormID)) {
                e->refined = true;
                e->note    = a_note;
            }
        }
        return true;
    }

    void Discard(const Entry& a_entry)
    {
        if (!a_entry.stagingDir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(StagingDirOf(a_entry.stagingDir), ec);
            if (ec) {
                logs::warn("staging: could not remove {} - {}"sv, a_entry.stagingDir, ec.message());
            }
        }

        std::lock_guard lock{ g_mutex };
        g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(),
                                       [&](const Entry& e) {
                                           return e.refFormID == a_entry.refFormID;
                                       }),
                        g_entries.end());
        logs::info("staging: discarded {}"sv, a_entry.name);
    }

    void ClearStaged()
    {
        const auto root = StagingRoot();

        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) {
            return;
        }

        const auto removed = std::filesystem::remove_all(root, ec);
        if (ec) {
            logs::warn("staging: could not clear {} - {}"sv, root.string(), ec.message());
            return;
        }
        if (removed > 0) {
            logs::info("staging: cleared {} leftover file(s) from the previous session"sv,
                       static_cast<std::uint64_t>(removed));
        }
    }

    bool Commit(const Entry& a_entry)
    {
        const auto source = StagingDirOf(a_entry.stagingDir) / "bio.prompt";
        {
            std::error_code ec;
            if (a_entry.stagingDir.empty() || !std::filesystem::exists(source, ec)) {
                std::lock_guard lock{ g_mutex };
                if (Entry* e = FindLocked(a_entry.refFormID)) {
                    e->note = "staged bio.prompt not found: " + ContentLibrary::Utf8Of(source);
                }
                return false;
            }
        }

        // Resolve the UUID here, on the calling (UI) thread: the worker that
        // follows must stay free of game-data reads, and this export is a
        // cheap table lookup. 0 is acceptable - the commit then relies on the
        // update-by-path half of the web call.
        const auto uuid = SN::FormIDToUUID(a_entry.refFormID);

        std::string stem{ a_entry.fileName };
        if (stem.ends_with(".prompt")) {
            stem.resize(stem.size() - 7);
        }
        const std::string name{ a_entry.name };
        const std::string stagingDir{ a_entry.stagingDir };

        {
            std::lock_guard lock{ g_mutex };
            Entry*          e = FindLocked(a_entry.refFormID);
            if (!e) {
                return false;
            }
            if (e->committing) {
                return true;   // a press already in flight; let it land
            }
            e->committing = true;
            e->note        = "committing...";
        }

        // The HTTP round trip (plus SkyrimNet's content rescan behind it) can
        // take seconds - exactly the class of call that once froze the game
        // when Commit ran it on the UI thread. Fire and forget: the worker
        // records the outcome under the mutex and the next Snapshot shows it.
        std::thread{ [refFormID = a_entry.refFormID, name, stem, stagingDir, uuid]() {
            const auto    bio    = ReadWholeFile(StagingDirOf(stagingDir) / "bio.prompt");
            const auto    result = Web::CreateOrUpdateBio(name, uuid, stem, bio);

            std::lock_guard lock{ g_mutex };
            if (Entry* e = FindLocked(refFormID)) {
                e->committing = false;
                e->committed  = result.ok;
                e->note       = result.note;
                if (result.ok) {
                    logs::info("commit: {} -> {}"sv, name, result.note);
                } else {
                    logs::error("commit: {} failed - {}"sv, name, result.note);
                }
            }
        } }.detach();

        return true;
    }
}
