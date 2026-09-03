"""Score and rank digest candidates for a settlement, offline.

The digest harvest picks which of the user's installed bios get offered to the
LLM as "who lives here". Getting that ranking right is pure text scoring over
the corpus, so it needs no game running - which makes it tunable in seconds
instead of rebuild / stage / launch / generate.

This mirrors RegionDigest::GatherCandidates. Keep the weights below in step
with the constants in RegionDigest.cpp; this file is where they get decided.

  python rank_probe.py Riften "The Rift"
  python rank_probe.py Ivarstead "The Rift" --top 40
  python rank_probe.py Riften "The Rift" --explain Wilhelm
"""
import argparse
import os
import re
import sys

CORPUS = os.environ.get(
    "BIOFORGE_CORPUS",
    r"F:\Modding\JOJ\overwrite\skse\plugins\SkyrimNet\prompts\characters")

# ---- weights (mirror RegionDigest.cpp) ------------------------------------
IN_SUMMARY, IN_BODY = 1000, 100
HOLD_IN_SUMMARY, HOLD_IN_BODY = 20, 1
LOCATIVE, EARLY = 40, 20
PAREN_PENALTY, OTHER_FIRST_PENALTY, ORIGIN_PENALTY = -600, -600, -150
# Citations outrank phrasing on purpose. A character the rest of the corpus
# actually refers to matters more than one whose summary happens to say
# "in Riften" rather than "of Riften" - the first cut had that backwards and
# dropped the Ragged Flagon's bouncer for a zero-citation passer-by.
CITE_WEIGHT, CITE_CAP = 12, 60
EARLY_FRACTION = 0.35

# Vanilla settlements, for the "names somewhere else first" penalty. Mod
# settlements are not here and simply do not get the penalty - it only ever
# demotes, so an unlisted place costs accuracy, never correctness.
SETTLEMENTS = ["Whiterun", "Solitude", "Markarth", "Windhelm", "Dawnstar",
               "Morthal", "Falkreath", "Winterhold", "Riften", "Ivarstead",
               "Rorikstead", "Kynesgrove", "Shor's Stone", "Riverwood",
               "Dragon Bridge", "Karthwasten", "Helgen", "Solstheim",
               "Raven Rock", "Darkwater Crossing", "Old Hroldan"]

BLOCK = re.compile(r"\{%\s*block\s+(\w+)\s*%\}([\s\S]*?)\{%\s*endblock")


def name_from_stem(stem):
    u = stem.rfind("_")
    if u != -1 and len(stem) - u <= 5:
        stem = stem[:u]
    out, cap = [], True
    for ch in stem:
        c = " " if ch == "_" else ch
        out.append(c.upper() if (cap and "a" <= c <= "z") else c)
        cap = c in (" ", "-")
    return "".join(out)


def load(corpus_dir):
    docs = []
    for e in os.scandir(corpus_dir):
        if not e.is_file() or not e.name.endswith(".prompt"):
            continue
        t = open(e.path, encoding="utf-8", errors="replace").read()
        b = dict(BLOCK.findall(t))
        stem = os.path.splitext(e.name)[0]
        docs.append({
            "stem": stem,
            "name": name_from_stem(stem),
            "text": t,
            "summary": " ".join(b.get("summary", "").split()),
            "rel": " ".join(b.get("relationships", "").split()),
        })
    return docs


def score(d, region, hold):
    """Returns (score, [reasons]) or (None, _) when not a candidate at all."""
    why = []
    region_hits = d["text"].count(region)
    hold_hits = d["text"].count(hold) if hold else 0
    if not region_hits and not hold_hits:
        return None, why

    s, summary = 0, d["summary"]
    if region_hits:
        if region in summary:
            s += IN_SUMMARY
            why.append("summary+%d" % IN_SUMMARY)
        else:
            s += IN_BODY
            why.append("body+%d" % IN_BODY)
    else:
        if hold and hold in summary:
            s += HOLD_IN_SUMMARY
            why.append("hold-summary+%d" % HOLD_IN_SUMMARY)
        else:
            s += HOLD_IN_BODY
            why.append("hold-body+%d" % HOLD_IN_BODY)

    i = summary.find(region)
    if i != -1:
        before = summary[max(0, i - 12):i].lower()
        if re.search(r"\b(in|at|of|near)\s+$", before) or \
           summary[i:i + len(region) + 2].startswith(region + "'s"):
            s += LOCATIVE
            why.append("locative+%d" % LOCATIVE)
        if i / max(len(summary), 1) < EARLY_FRACTION:
            s += EARLY
            why.append("early+%d" % EARLY)

        opened, closed = summary.rfind("(", 0, i), summary.rfind(")", 0, i)
        if opened > closed:
            s += PAREN_PENALTY
            why.append("parenthetical%d" % PAREN_PENALTY)

        first_other = [o for o in SETTLEMENTS
                       if o != region and o in summary and summary.find(o) < i]
        if first_other:
            s += OTHER_FIRST_PENALTY
            why.append("after-%s%d" % (first_other[0], OTHER_FIRST_PENALTY))

        # "from Riften" is origin; "from Riften's docks" is locative-possessive.
        if re.search(r"\bfrom\s+$", before) and \
           not summary[i:].startswith(region + "'s"):
            s += ORIGIN_PENALTY
            why.append("origin%d" % ORIGIN_PENALTY)

    return s, why


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("region")
    ap.add_argument("hold", nargs="?", default="")
    ap.add_argument("--top", type=int, default=120)
    ap.add_argument("--show", type=int, default=25)
    ap.add_argument("--explain", default=None)
    ap.add_argument("--corpus", default=CORPUS)
    a = ap.parse_args()

    docs = load(a.corpus)
    print("corpus: %d bios  |  region=%r hold=%r  cap=%d\n"
          % (len(docs), a.region, a.hold, a.top))

    cands = []
    for d in docs:
        sc, why = score(d, a.region, a.hold)
        if sc is not None:
            cands.append({"d": d, "s": sc, "why": why, "cites": 0})

    # Co-citation over the CANDIDATES' relationships blocks, not the whole
    # corpus. Measured against the full-corpus version: the kept 120 differs by
    # a single bio, for a tenth of the text to scan - and locals naming locals
    # is arguably the truer signal of who matters here. RegionDigest.cpp does
    # exactly this; keep the two in step.
    ties = "\n".join(c["d"]["rel"] for c in cands)
    for c in cands:
        n = c["d"]["name"]
        if " " in n or len(n) >= 5:
            c["cites"] = max(0, ties.count(n) - c["d"]["rel"].count(n))
        bonus = CITE_WEIGHT * min(c["cites"], CITE_CAP)
        if bonus:
            c["why"].append("cites=%d+%d" % (c["cites"], bonus))
        c["s"] += bonus

    cands.sort(key=lambda c: -c["s"])

    # One slot per character: the corpus can hold two files for the same
    # person (different reference FormIDs), and offering the model the same
    # name twice buys nothing.
    seen, deduped = set(), []
    for c in cands:
        key = c["d"]["name"].lower()
        if key in seen:
            continue
        seen.add(key)
        deduped.append(c)
    dropped_dupes = len(cands) - len(deduped)
    cands = deduped
    kept = cands[:a.top]
    if dropped_dupes:
        print("(deduped %d repeat name(s))" % dropped_dupes)

    if a.explain:
        print("=== explain: names matching %r ===" % a.explain)
        for rank, c in enumerate(cands, 1):
            if a.explain.lower() in c["d"]["name"].lower():
                print("  rank %-4d score %-6d %-24s %s"
                      % (rank, c["s"], c["d"]["name"][:24], ", ".join(c["why"])))
                print("      %s" % c["d"]["summary"][:150])
                print("      %s" % ("KEPT" if rank <= a.top else "dropped"))
        return

    print("=== top %d of %d candidates ===" % (min(a.show, len(kept)), len(cands)))
    for rank, c in enumerate(kept[:a.show], 1):
        print("  %3d %6d  %-26s %-4s %s"
              % (rank, c["s"], c["d"]["name"][:26], c["cites"],
                 c["d"]["summary"][:74]))

    print("\n=== last 5 kept (the cut line) ===")
    for rank, c in enumerate(kept[-5:], len(kept) - 4):
        print("  %3d %6d  %-26s %-4s %s"
              % (rank, c["s"], c["d"]["name"][:26], c["cites"],
                 c["d"]["summary"][:74]))

    print("\n=== first 5 dropped ===")
    for rank, c in enumerate(cands[a.top:a.top + 5], a.top + 1):
        print("  %3d %6d  %-26s %-4s %s"
              % (rank, c["s"], c["d"]["name"][:26], c["cites"],
                 c["d"]["summary"][:74]))

    demoted = [c for c in cands if any("parenthetical" in w or "after-" in w
                                       or "origin" in w for w in c["why"])]
    print("\n=== demoted by a penalty: %d  (kept anyway: %d) ==="
          % (len(demoted), sum(1 for c in demoted if c in kept)))
    for c in demoted[:8]:
        print("  %6d  %-24s %-28s %s"
              % (c["s"], c["d"]["name"][:24],
                 ",".join(w for w in c["why"] if "-" in w or "penal" in w)[:28],
                 c["d"]["summary"][:60]))

    scores = [c["s"] for c in kept]
    print("\n=== tie health ===")
    print("  distinct scores among the kept %d: %d   (was 2 before ranking)"
          % (len(kept), len(set(scores))))


if __name__ == "__main__":
    main()
