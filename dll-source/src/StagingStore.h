#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace BioForge
{
    struct Candidate;

    namespace Staging
    {
        // The ten blocks a character bio is made of, in the order
        // dynamic_character_bio consumes them. The LLM is instructed to emit
        // each under a `### <name>` heading; ParseResponse maps those headings
        // onto these blocks.
        inline constexpr std::string_view kBlockNames[10] = {
            "summary",    "interject_summary", "background",   "personality",
            "appearance", "aspirations",       "relationships", "occupation",
            "skills",     "speech_style"
        };

        enum class State
        {
            Generating,
            Staged,
            Failed
        };

        struct Entry
        {
            std::uint32_t refFormID{};
            std::string   name;          // NPC display name
            std::string   fileName;      // "gudra_59A6.prompt" - target under characters/
            std::string   stagingDir;    // bundle dir under bioforge_staging/
            State         state{ State::Generating };
            std::string   note;          // failure reason / parse gaps / commit result
            bool          committed{};
        };

        // <Data>/SKSE/Plugins/SkyrimNet/prompts, resolved from this DLL's own
        // location. Reads and writes go through the game process's USVFS view,
        // so staged files land where SkyrimNet can actually open them (files
        // created by an EXTERNAL process while the game runs are enumerated by
        // SkyrimNet's path scan but fail to open).
        std::filesystem::path PromptsDir();

        // Target bio file name for a candidate. Prefers SkyrimNet's own
        // resolution (it may disambiguate); otherwise derives the corpus
        // convention: lowercase, spaces -> '_', anything outside [a-z0-9_-]
        // dropped, then '_' + low 12 bits of the REFERENCE FormID as three
        // uppercase hex digits (torg_strong-arm_9A8 <- 0x680059A8).
        std::string BioFileName(const Candidate& a_candidate);

        // Parse an LLM response into the ten blocks and render it as a
        // {% block %} character bio in the corpus format. Lines before the
        // first `###` heading are treated as preamble and dropped; ``` fences
        // are stripped. False when any block is missing or empty - `a_missing`
        // names which.
        bool ParseResponse(std::string_view a_raw, std::string& a_bioText,
                           std::vector<std::string>& a_missing);

        // --- lifecycle, callable from any thread ---

        // Register an in-flight generation (called when the LLM task is queued).
        void Begin(std::uint32_t a_refFormID, std::string_view a_name,
                   std::string_view a_fileName);

        // Complete an entry from the LLM worker thread. Pure file I/O - no
        // RE:: calls. Writes the staging bundle (bio.prompt on a clean parse,
        // plus response.raw.txt and harvest.json either way, so a bad response
        // can still be inspected and retried). Empty a_bioText marks the entry
        // Failed with the missing blocks named in a_missing.
        void RecordGenerated(std::uint32_t a_refFormID, std::string_view a_contextJson,
                             std::string_view a_rawResponse, const std::string& a_bioText,
                             const std::vector<std::string>& a_missing);

        // Fail an entry (queue refusal, LLM error, no UUID...). a_reason is the
        // error string, possibly SkyrimNet's own text.
        void RecordFailed(std::uint32_t a_refFormID, std::string_view a_reason);

        // Thread-safe copy of all entries, oldest first, for the UI.
        std::vector<Entry> Snapshot();

        // Delete every staged bundle. Staging is per-session scratch: a bio that
        // was not committed is not in use by anything, and the review list it
        // belonged to died with the process. Called once as the game loads,
        // rather than on exit - Skyrim has no reliable shutdown hook, and file
        // I/O from DllMain during teardown is not worth the risk. Clearing on
        // the way in gives the same guarantee and leaves the last session's
        // bundles readable in the meantime, which is useful when a generation
        // went wrong and you want to see the prompt that produced it.
        // Committed bios live in prompts/characters and are never touched.
        void ClearStaged();

        // Move a staged bio into prompts/characters/. Any existing file is
        // backed up first as <name>.prompt.backup.<unixtime> (SkyrimNet's own
        // convention), then the prompt cache is reloaded best-effort so the bio
        // resolves without a restart. Returns false with a_note on failure.
        bool Commit(const Entry& a_entry, std::string& a_note);

        // Pull the body of a bio's `summary` block out of .prompt text. Empty
        // when there is no such block.
        std::string ExtractSummary(std::string_view a_promptText);

        // One line describing who this actor already is, for the roster handed
        // to a neighbour's generation: the first sentence of their summary
        // block. Checks THIS SESSION'S staging bundle first (a batch-mate
        // generated minutes ago is the freshest truth), then the committed
        // corpus. Empty when nobody has written them yet.
        //
        // This matters more than it looks. Without it the roster is bare names
        // and the model invents what each neighbour is like - which is how a
        // paying guest became a housemate and a girl from Ivarstead became a
        // resident, both of whom already had bios saying otherwise.
        std::string BioSummary(const Candidate& a_candidate);

        // The staged bio as written, for the review panel. Empty if the bundle
        // has no bio (a failed parse) or cannot be read.
        std::string ReadStagedBio(const Entry& a_entry);

        // The raw LLM response, for when a parse failed and the reason matters.
        std::string ReadRawResponse(const Entry& a_entry);

        // Reject a staged bio: delete its bundle and drop it from the review
        // list. Does NOT touch an already-committed file in prompts/characters
        // - undoing a commit means restoring its .prompt.backup.<time>.
        void Discard(const Entry& a_entry);
    }
}
