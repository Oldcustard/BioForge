#include "pch.h"

#include "Generator.h"

#include "Config.h"
#include "Json.h"
#include "RegionDigest.h"
#include "ScopeSelector.h"
#include "StagingStore.h"
#include "SkyrimNetAPI.h"

#include <deque>
#include <mutex>
#include <utility>

namespace BioForge::Generator
{
    namespace
    {
        constexpr auto kPromptName = "bioforge_generate"sv;

        // SkyrimNet's own bio-writing variant, not one of ours. Bio Forge does
        // exactly the job this variant is already configured for, so it should
        // inherit whatever model the user picked for profile generation rather
        // than making them configure a second one. It is also tuned for the
        // task - full model, 10k max_tokens, temperature 0.7 - where a fresh
        // variant inherits the dialogue defaults (a flash model on a 4k cap,
        // which is tight for a ten-block bio).
        constexpr auto kVariant = "CharacterProfileGeneration"sv;

        // Pull the string values of one key out of a flat JSON array of
        // objects. Deliberately a scanner and not a parser: the two payloads
        // consumed here (PublicGetRelatedActors, PublicGetWorldKnowledgeForActor)
        // are flat and fixed-shape, and a JSON library would otherwise be this
        // plugin's only dependency.
        std::vector<std::string> ExtractValues(std::string_view a_json, std::string_view a_key)
        {
            std::vector<std::string> out;

            const std::string needle = std::string{ "\"" } + std::string{ a_key } + "\":\"";
            std::size_t       pos    = 0;
            while ((pos = a_json.find(needle, pos)) != std::string_view::npos) {
                pos += needle.size();

                std::string value;
                while (pos < a_json.size() && a_json[pos] != '"') {
                    if (a_json[pos] == '\\' && pos + 1 < a_json.size()) {
                        switch (a_json[pos + 1]) {
                        case 'n':  value += ' ';  break;   // keep each entry on one line
                        case 't':  value += ' ';  break;
                        case '"':  value += '"';  break;
                        case '\\': value += '\\'; break;
                        case '/':  value += '/';  break;
                        default:   break;                  // \uXXXX and friends: drop
                        }
                        pos += 2;
                        continue;
                    }
                    value += a_json[pos++];
                }
                ++pos;   // past the closing quote

                if (!value.empty()) {
                    out.push_back(std::move(value));
                }
            }
            return out;
        }

        // Render extracted values as markdown bullets. Empty yields "" so the
        // template drops the whole section: a line starting with '[' (raw JSON,
        // including "[]") makes SkyrimNet's renderer strip the heading above it.
        std::string ToBullets(const std::vector<std::string>& a_values)
        {
            std::string out;
            for (const auto& v : a_values) {
                out += "- ";
                out += v;
                out += "\n";
            }
            if (!out.empty()) {
                out.pop_back();
            }
            return out;
        }

        // The harvest the DLL side adds. Everything else (dialogue, stats,
        // equipment, location) the template pulls itself through decorators -
        // that is what keeps prompt iteration rebuild-free.
        std::string BuildContext(const Candidate&              a_candidate,
                                 const std::vector<Candidate>& a_roster,
                                 std::string_view              a_regionDigest)
        {
            const auto uuid = SN::FormIDToUUID(a_candidate.refFormID);
            if (uuid == 0) {
                logs::error("generate: SkyrimNet has no UUID for {:08X} - not tracked yet?"sv,
                            a_candidate.refFormID);
                return {};
            }

            // A JSON NUMBER, not a string: the decorators call get<uint64_t>()
            // on this and throw "type must be number, but is string" for a hex
            // string. (The dashboard's render-template-preview endpoint is the
            // opposite - it takes hex text and converts it before binding.)
            const auto decimal = std::to_string(uuid);

            // Both arrive as JSON. Convert to bullets here rather than dumping
            // the raw array into the prompt: it reads better, and a '[' at the
            // start of a line costs the section its heading.
            const auto related =
                ToBullets(ExtractValues(SN::RelatedActors(a_candidate.refFormID, 8), "name"));
            const auto world =
                ToBullets(ExtractValues(SN::WorldKnowledgeForActor(a_candidate.refFormID, 5),
                                        "content"));

            // Everyone else standing in the same place, so relationships can
            // name real people. Followers are marked: they are only here
            // because the player walked them in, and writing them into a
            // resident's life would be wrong.
            std::string roster;
            for (const auto& other : a_roster) {
                if (other.refFormID == a_candidate.refFormID || other.name.empty()) {
                    continue;
                }
                roster += "- " + other.name;
                if (!other.race.empty()) {
                    roster += " (" + other.race + ")";
                }
                if (other.isFollower) {
                    roster += " - travelling with the player, not a local";
                }
                roster += '\n';
            }
            if (!roster.empty()) {
                roster.pop_back();
            }

            return std::string{ "{" }
                   + "\"actorUUID\":" + decimal + ","
                   + "\"localActors\":\"" + Json::Escape(roster) + "\","
                   + "\"sourcePlugin\":\"" + Json::Escape(a_candidate.sourcePlugin) + "\","
                   + "\"relatedActors\":\"" + Json::Escape(related) + "\","
                   + "\"worldKnowledge\":\"" + Json::Escape(world) + "\","
                   // Who matters in this settlement, written once and reused by
                   // every bio generated here. Already bullet lines, so it is
                   // safe to drop straight into a heading's body.
                   + "\"regionDigest\":\"" + Json::Escape(a_regionDigest) + "\"}";
        }
    }

    namespace
    {
        // A job carries everything the dispatch needs, assembled in advance on
        // the main thread. That is the point: the completion callback runs on a
        // SkyrimNet ThreadPool worker and must never reach into game data, so
        // nothing here may require a second look at the world.
        struct Job
        {
            std::uint32_t refFormID{};
            std::string   name;
            std::string   fileName;
            std::string   contextJson;
        };

        std::mutex      g_queueMutex;
        std::deque<Job> g_queue;
        int             g_inFlight = 0;

        // A batch held back until its region digest lands. These stay
        // Candidates rather than Jobs because assembling a job reads game
        // data, which only the UI thread may do - see Tick().
        std::vector<Candidate> g_pending;
        std::vector<Candidate> g_pendingRoster;
        std::string            g_pendingRegion;

        void Pump();

        void OnComplete(std::uint32_t a_refFormID, const std::string& a_context,
                        const char* a_response, int a_success)
        {
            if (!a_response || a_success == 0) {
                Staging::RecordFailed(a_refFormID, a_response ? a_response : "empty response");
            } else {
                const std::string        raw{ a_response };   // pointer dies with the call
                std::string              bio;
                std::vector<std::string> missing;
                Staging::ParseResponse(raw, bio, missing);
                Staging::RecordGenerated(a_refFormID, a_context, raw, bio, missing);
            }

            {
                std::lock_guard lock{ g_queueMutex };
                --g_inFlight;
            }
            Pump();   // a slot just freed up
        }

        // Dispatch until the concurrency cap is reached or the queue empties.
        // Safe to call from the main thread or from a worker finishing a job.
        void Pump()
        {
            for (;;) {
                Job job;
                {
                    std::lock_guard lock{ g_queueMutex };
                    if (g_queue.empty() || g_inFlight >= Config::Get().maxConcurrent) {
                        return;
                    }
                    job = std::move(g_queue.front());
                    g_queue.pop_front();
                    ++g_inFlight;
                }

                Staging::Begin(job.refFormID, job.name, job.fileName);

                const bool queued = SN::SendCustomPrompt(
                    kPromptName.data(), kVariant.data(), job.contextJson.c_str(),
                    [ref = job.refFormID, ctx = job.contextJson](const char* a_response,
                                                                 int         a_success) {
                        OnComplete(ref, ctx, a_response, a_success);
                    });

                if (!queued) {
                    Staging::RecordFailed(job.refFormID, "SkyrimNet did not queue the task");
                    {
                        std::lock_guard lock{ g_queueMutex };
                        --g_inFlight;
                    }
                    continue;   // the loop takes the next one; no recursion
                }

                logs::info("generate: task queued for {} ({})"sv, job.name, job.fileName);
            }
        }

        // Main thread only - reads game data through the SkyrimNet API.
        bool BuildJob(const Candidate& a_candidate, const std::vector<Candidate>& a_roster,
                      std::string_view a_regionDigest, Job& a_job)
        {
            const auto context = BuildContext(a_candidate, a_roster, a_regionDigest);
            if (context.empty()) {
                return false;
            }

            a_job.refFormID   = a_candidate.refFormID;
            a_job.name        = a_candidate.name;
            a_job.fileName    = Staging::BioFileName(a_candidate);
            a_job.contextJson = context;
            return true;
        }

        // Build first, lock second. Assembling a job calls into SkyrimNet for
        // related actors and world knowledge, and holding the queue lock across
        // that would stall the workers trying to pick up their next job.
        std::size_t QueueJobs(const std::vector<Candidate>& a_candidates,
                              const std::vector<Candidate>& a_roster,
                              std::string_view              a_regionDigest)
        {
            std::vector<Job> jobs;
            jobs.reserve(a_candidates.size());
            for (const auto& c : a_candidates) {
                Job job;
                if (BuildJob(c, a_roster, a_regionDigest, job)) {
                    jobs.push_back(std::move(job));
                }   // otherwise BuildContext has already logged why
            }

            const std::size_t queued = jobs.size();
            {
                std::lock_guard lock{ g_queueMutex };
                for (auto& job : jobs) {
                    g_queue.push_back(std::move(job));
                }
            }
            Pump();
            return queued;
        }

        bool Ready()
        {
            if (!SN::CanGenerate()) {
                logs::error("generate: SkyrimNet API is older than v8 or absent"sv);
                return false;
            }
            if (!SN::MemorySystemReady()) {
                logs::error("generate: memory system not ready - load a save first"sv);
                return false;
            }
            return true;
        }
    }

    bool Generate(const Candidate& a_candidate, const std::vector<Candidate>& a_roster)
    {
        if (!Ready()) {
            return false;
        }

        // Whatever digest is already cached, and no more. A single generation
        // deliberately does not build one: that is a second LLM call the user
        // did not ask for, and the digest only pays for itself amortised over
        // a batch. The panel shows whether one exists, and offers the button.
        std::string digest;
        if (Config::Get().digestEnabled) {
            digest = RegionDigest::Get(RegionDigest::Current().name);
        }

        Job job;
        if (!BuildJob(a_candidate, a_roster, digest, job)) {
            return false;
        }

        {
            std::lock_guard lock{ g_queueMutex };
            g_queue.push_back(std::move(job));
        }
        Pump();
        return true;
    }

    std::size_t GenerateAll(const std::vector<Candidate>& a_candidates,
                            const std::vector<Candidate>& a_roster)
    {
        if (!Ready()) {
            return 0;
        }

        std::string digest;
        if (Config::Get().digestEnabled) {
            const auto region = RegionDigest::Current();
            if (region.Valid()) {
                digest = RegionDigest::Get(region.name);

                // Nothing cached for this settlement. Running the batch now
                // would quietly produce a whole cell's worth of bios that all
                // lack the local knowledge the digest exists to supply, so hold
                // the candidates and let Tick() queue them once it lands.
                // Building() covers the case where the user already pressed the
                // digest button and the batch button straight after.
                if (digest.empty() && Config::Get().digestAutoBuild &&
                    (RegionDigest::Building() || RegionDigest::Build(region))) {
                    std::lock_guard lock{ g_queueMutex };
                    g_pending       = a_candidates;
                    g_pendingRoster = a_roster;
                    g_pendingRegion = region.name;
                    logs::info("generate: {} candidate(s) waiting on the region digest for {}"sv,
                               a_candidates.size(), region.name);
                    return 0;
                }
            }
        }

        const auto queued = QueueJobs(a_candidates, a_roster, digest);
        logs::info("generate: queued {} of {} candidate(s) for batch generation"sv,
                   queued, a_candidates.size());
        return queued;
    }

    void Tick()
    {
        std::vector<Candidate> batch;
        std::vector<Candidate> roster;
        std::string            region;
        {
            std::lock_guard lock{ g_queueMutex };
            if (g_pending.empty() || RegionDigest::Building()) {
                return;   // nothing held, or still waiting - the common case
            }
            batch  = std::exchange(g_pending, {});
            roster = std::exchange(g_pendingRoster, {});
            region = std::exchange(g_pendingRegion, {});
        }

        // The build may have failed or come back unusable. Say so plainly and
        // generate anyway - a bio without the local reference sheet is still a
        // great deal better than no bio.
        const auto digest = RegionDigest::Get(region);
        if (digest.empty()) {
            logs::warn("generate: no digest for {} - writing {} bio(s) without one"sv,
                       region, batch.size());
        }

        const auto queued = QueueJobs(batch, roster, digest);
        logs::info("generate: queued {} of {} candidate(s) held for the {} digest"sv,
                   queued, batch.size(), region);
    }

    Progress GetProgress()
    {
        std::lock_guard lock{ g_queueMutex };

        Progress progress;
        progress.inFlight      = g_inFlight;
        progress.queued        = static_cast<int>(g_queue.size());
        progress.pending       = static_cast<int>(g_pending.size());
        progress.pendingRegion = g_pendingRegion;
        return progress;
    }

    void CancelQueued()
    {
        std::size_t dropped = 0;
        {
            std::lock_guard lock{ g_queueMutex };
            dropped = g_queue.size() + g_pending.size();
            g_queue.clear();
            g_pending.clear();
            g_pendingRoster.clear();
            g_pendingRegion.clear();
        }
        if (dropped > 0) {
            logs::info("generate: dropped {} queued job(s); in-flight requests still finish"sv,
                       dropped);
        }
    }
}
