#include "pch.h"

#include "StagingStore.h"

#include "PromptReload.h"
#include "ScopeSelector.h"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>

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
            return PromptsDir() / "bioforge_staging" / stem;
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

    std::filesystem::path PromptsDir()
    {
        // Resolve from this DLL's own module: the game process's USVFS view of
        // Data/SKSE/Plugins is exactly what SkyrimNet reads and writes through,
        // which is the whole point of committing from in here.
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&PromptsDir), &module);

        wchar_t buffer[MAX_PATH * 2]{};
        if (!module || !GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)))) {
            logs::error("staging: cannot locate own module - falling back to relative path"sv);
            return std::filesystem::path{ "Data/SKSE/Plugins/SkyrimNet/prompts" };
        }

        // <...>/SKSE/Plugins/BioForge.dll -> <...>/SKSE/Plugins/SkyrimNet/prompts
        return std::filesystem::path{ buffer }.parent_path() / "SkyrimNet" / "prompts";
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

        std::string derived;
        for (const char ch : a_candidate.name) {
            const auto c = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (c == ' ') {
                derived += '_';
            } else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
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
            e->stagingDir = bundle.string();
        }

        // The bundle is the audit trail: what the model was asked (harvest),
        // what it answered (raw), and what Bio Forge would commit (bio).
        WriteFile(bundle / "harvest.json", a_contextJson);
        WriteFile(bundle / "response.raw.txt", a_rawResponse);

        if (!a_bioText.empty()) {
            WriteFile(bundle / "bio.prompt", a_bioText);
        }

        std::lock_guard lock{ g_mutex };
        if (Entry* e = FindLocked(a_refFormID)) {
            logs::info("generate: {} {} (bundle: {})"sv, e->name,
                       e->state == State::Staged ? "staged"sv : "failed parse"sv,
                       bundle.string());
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

    std::string ReadStagedBio(const Entry& a_entry)
    {
        if (a_entry.stagingDir.empty()) {
            return {};
        }
        return ReadWholeFile(std::filesystem::path{ a_entry.stagingDir } / "bio.prompt");
    }

    std::string ReadRawResponse(const Entry& a_entry)
    {
        if (a_entry.stagingDir.empty()) {
            return {};
        }
        return ReadWholeFile(std::filesystem::path{ a_entry.stagingDir } / "response.raw.txt");
    }

    void Discard(const Entry& a_entry)
    {
        if (!a_entry.stagingDir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(std::filesystem::path{ a_entry.stagingDir }, ec);
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
        const auto root = PromptsDir() / "bioforge_staging";

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

    bool Commit(const Entry& a_entry, std::string& a_note)
    {
        // SkyrimNet resolves bios by TEMPLATE NAME (no extension), but the file
        // on disk must still be <name>.prompt or nothing will ever find it.
        std::string file{ a_entry.fileName };
        if (!file.ends_with(".prompt")) {
            file += ".prompt";
        }

        const auto target = PromptsDir() / "characters" / file;
        const auto source = std::filesystem::path{ a_entry.stagingDir } / "bio.prompt";

        {
            std::error_code ec;
            if (!std::filesystem::exists(source, ec)) {
                a_note = "staged bio.prompt not found: " + source.string();
                return false;
            }
        }

        // Backup any existing file first, SkyrimNet's own convention.
        {
            std::error_code ec;
            std::filesystem::create_directories(target.parent_path(), ec);
            if (std::filesystem::exists(target, ec)) {
                const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
                const auto backup =
                    target.string() + ".backup." + std::to_string(secs);
                std::filesystem::rename(target, backup, ec);
                if (ec) {
                    a_note = "could not back up existing file: " + ec.message();
                    return false;
                }
            }
        }

        {
            std::error_code ec;
            std::filesystem::copy_file(source, target,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                a_note = "copy failed: " + ec.message();
                return false;
            }
        }

        a_note = "committed " + target.string();

        // New .prompt files are invisible to SkyrimNet's path cache until a
        // reload - best-effort, and honestly reported when unreachable.
        const auto reload = PromptReload::Request();
        if (!reload.empty()) {
            a_note += " (prompt cache not reloaded: " + reload + ")";
        }

        {
            std::lock_guard lock{ g_mutex };
            if (Entry* e = FindLocked(a_entry.refFormID)) {
                e->committed = true;
                e->note      = a_note;
            }
        }
        logs::info("commit: {}"sv, a_note);
        return true;
    }
}
