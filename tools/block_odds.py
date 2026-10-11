"""How often each bio block is ROLLED for a given kind of NPC.

Tune block_rules.ini here, not in the game. This replays BlockRules::Pick
for a profile you describe - facts, `chance`, weights, `rarely`, `unless`,
merged keys and pools - and prints the exact odds of every block before the
model's review. Every group rolls independently (there is no cap), so the odds are
worked out, not simulated. The review can only lower a roll's odds (a
contradicted roll is replaced or dropped) or add a named one, so these are
the ceiling for anything random.

    python tools/block_odds.py --race NordRace
    python tools/block_odds.py --race NordRace --group religion
    python tools/block_odds.py --race OrcRace --faction "Skyrim.esm|086EEE" --group temperament
    python tools/block_odds.py --race NordRace --match "religion\\.(namira|sanguine|boethiah|mephala|meridia|sheogorath|hermaeus-mora|clavicus-vile|vaermina|peryite|mehrunes-dagon|jyggalag|malacath|molag-bal|hircine|nocturnal|azura|sithis|alduin)$"

--match gives the odds of an NPC getting ANY block whose key matches the
regex: "how many NPCs end up a secret Daedra worshipper" rather than one
Prince at a time.

Not modelled: what the NPC already carries (a covered group is skipped in
game), blocks missing from SeverActions' library, and intimate tabs flagged
by SeverActions rather than by `intimate = true`.
"""

import argparse
import re
import sys
from pathlib import Path

RULES = Path(__file__).resolve().parent.parent / "mod-root/SKSE/Plugins/BioForge/block_rules.ini"


def parse(path):
    groups = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            groups.append({"name": line[1:-1].strip().lower(), "chance": None, "intimate": False,
                           "unless": [], "pools": {}, "entries": []})
            continue
        if "=" not in line or not groups:
            continue
        name, value = (s.strip() for s in line.split("=", 1))
        g = groups[-1]
        if name == "chance":
            g["chance"] = int(value)
        elif name == "intimate":
            g["intimate"] = value.lower() == "true"
        elif name == "unless":
            g["unless"] = value.split()
        elif name == "pool":
            pool, pct = value.split()
            g["pools"][pool.lower()] = min(max(float(pct), 0.0), 100.0) / 100.0
        elif name in ("fact", "flavour", "flavor"):
            tokens = value.split()
            e = {"key": tokens[0].lower(), "fact": name == "fact", "weight": 1, "pool": "",
                 "when": [], "unless": [], "rarely": []}
            mode = None
            i = 1
            while i < len(tokens):
                t = tokens[i].lower()
                if t == "weight":
                    e["weight"] = max(0, int(tokens[i + 1]))
                    mode = None
                    i += 2
                    continue
                if t == "pool":
                    e["pool"] = tokens[i + 1].lower()
                    mode = None
                    i += 2
                    continue
                if t in ("when", "unless", "rarely"):
                    mode = t
                elif mode:
                    e[mode].append(tokens[i])
                i += 1
            g["entries"].append(e)
    return groups


def holds(cond, npc):
    c = cond.lower()
    if c in ("child", "adult", "male", "female"):
        return {"child": npc.child, "adult": not npc.child,
                "male": not npc.female, "female": npc.female}[c]
    m = re.fullmatch(r"(\w+)\((.*)\)", cond)
    if not m:
        return False
    kind, arg = m.group(1).lower(), m.group(2)
    if kind == "faction":
        return arg.lower() in npc.factions
    if kind == "voice":
        return npc.voice is not None and arg.lower() == npc.voice
    if kind == "race":
        return npc.race.lower().startswith(arg.lower())
    if kind == "keyword":
        return arg.lower() in npc.keywords
    if kind == "skill":
        sm = re.fullmatch(r"(\w+)\s*>=\s*(\d+)", arg)
        return bool(sm) and npc.skills.get(sm.group(1).lower(), 0) >= int(sm.group(2))
    return False


def line_shares(pools, lines):
    """As BlockRules' LineShares: each pool takes its fixed slice of the
    group's rolls, split by weight among its usable members; everything else
    shares the rest by weight."""
    free_weight = sum(w for _, w, p in lines if not p)
    pool_weight = {}
    for _, w, p in lines:
        if p:
            pool_weight[p] = pool_weight.get(p, 0) + w
    pool_share = {p: s for p, s in pools.items() if pool_weight.get(p, 0) > 0}
    pooled = sum(pool_share.values())
    scale = 1.0 / pooled if pooled > 1.0 or (free_weight <= 0 and pooled > 0) else 1.0
    rest = 1.0 - pooled * scale
    out = []
    for _, w, p in lines:
        if not p:
            out.append(rest * w / free_weight if free_weight > 0 else 0.0)
        elif p in pool_share:
            out.append(pool_share[p] * scale * w / pool_weight[p])
        else:
            out.append(0.0)
    return out


def prepare(groups, npc):
    """Facts, and each open group's chance and per-block shares - Pick's
    deterministic half."""
    facts, open_groups = [], []
    for g in groups:
        if g["intimate"] and (not npc.intimate or npc.child):
            continue
        if any(holds(c, npc) for c in g["unless"]):
            continue
        usable = [e for e in g["entries"]
                  if all(holds(c, npc) for c in e["when"])
                  and not any(holds(c, npc) for c in e["unless"])]
        fact = next((e for e in usable if e["fact"]), None)
        if fact:
            facts.append((g["name"], fact["key"]))
            continue
        lines = []
        for e in usable:
            if e["fact"]:
                continue
            w = e["weight"]
            if w > 0 and any(holds(c, npc) for c in e["rarely"]):
                w = max(1, w // 10)          # a tenth, as BlockRules::Pick does
            lines.append((e["key"], w, e["pool"]))
        if not lines:
            continue
        shares = {}
        for (key, _, _), s in zip(lines, line_shares(g["pools"], lines)):
            shares[key] = shares.get(key, 0.0) + s
        # As BlockRules::Pick: an intimate group's own chance scales the
        # intimate setting (unset = 100); any other unset group never rolls.
        if g["intimate"]:
            own = 100 if g["chance"] is None else g["chance"]
            chance = npc.intimate_chance * own / 100.0
        else:
            chance = g["chance"] or 0
        if sum(shares.values()) <= 0:
            chance = 0
        open_groups.append((g["name"], chance, shares))
    return facts, open_groups


def odds(open_groups):
    """Each block's probability: the group lands, then its share."""
    return [(name, chance, {k: chance / 100.0 * s for k, s in shares.items()})
            for name, chance, shares in open_groups]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--race", default="NordRace")
    ap.add_argument("--faction", action="append", default=[], help='"Plugin.esm|FormID", repeatable')
    ap.add_argument("--voice", help='"Plugin.esm|FormID"')
    ap.add_argument("--keyword", action="append", default=[])
    ap.add_argument("--skill", action="append", default=[], help="Destruction=50, repeatable")
    ap.add_argument("--child", action="store_true")
    ap.add_argument("--female", action="store_true")
    ap.add_argument("--intimate", action="store_true")
    ap.add_argument("--intimate-chance", type=int, default=50)
    ap.add_argument("--group", help="list only this group")
    ap.add_argument("--match", help="regex over keys: odds of getting ANY matching block")
    ap.add_argument("--rules", type=Path, default=RULES)
    npc = ap.parse_args()
    npc.factions = {f.lower() for f in npc.faction}
    npc.keywords = {k.lower() for k in npc.keyword}
    npc.voice = npc.voice.lower() if npc.voice else None
    npc.skills = {k.lower(): int(v) for k, v in (s.split("=") for s in npc.skill)}

    facts, open_groups = prepare(parse(npc.rules), npc)
    groups = odds(open_groups)

    def pct(p):
        if p <= 0:
            return "   never"
        n = 1 / p
        return f"{100 * p:6.2f}%  (1 in {n:,.1f})" if n < 10 else f"{100 * p:6.2f}%  (1 in {n:,.0f})"

    for group, key in facts:
        print(f"fact   {group:<16} {key.split(':')[-1]}")
    if facts:
        print()

    for name, chance, p in groups:
        if npc.group and name != npc.group:
            continue
        print(f"[{name}] lands for {pct(chance / 100.0)}")
        for k in sorted(p, key=lambda k: -p[k]):
            print(f"   {pct(p[k])}  {k.split(':')[-1]}")
        print()

    expected = sum(chance for _, chance, _ in groups) / 100.0
    print(f"flavour blocks per NPC: {expected:.2f} on average, plus {len(facts)} fact(s)")

    if npc.match:
        rx = re.compile(npc.match)
        none = 1.0
        for _, _, p in groups:
            none *= 1.0 - sum(v for k, v in p.items() if rx.search(k))
        print(f"any block matching {npc.match}: {pct(1.0 - none)} of NPCs")


if __name__ == "__main__":
    sys.exit(main())
