"""Probe SkyrimNet's render-template-preview with the game running.

Originally written to isolate the vanishing `## Notable skills` heading in
bioforge_generate.prompt; the G/H cases are that investigation, kept because
they document the heading-drop rule that still governs every edit to these
templates. The D cases cover the regional digest section.

`live_generate` / `live_digest` render the SHIPPED template files by name
rather than a synthetic snippet, which is the check that actually matters
before a release.

Usage: python render_probe.py [case ...]   (default: all cases)
"""
import json
import sys
import urllib.error
import urllib.request

URL = "http://127.0.0.1:8080/prompts?api=render-template-preview"
TORG = "0CDD2B350A026B6D"  # hex-string form; decimal string renders FFFF...

SETUP = "{% set npc = decnpc(actorUUID) %}\n"

# The digest section as it is shipped, trailed by the heading that follows
# it in the real template. The point of the D cases is that heading: a
# section whose body renders empty takes the heading above it with it, and
# can swallow the next one too.
DIGEST_BLOCK = (
    "{% if length(regionDigest) > 0 %}## Who matters around here\n"
    "{{ regionDigest }}\n"
    "Use it to ground relationships in real local names.\n"
    "\n"
    "{% endif %}## Their own dialogue\n"
    "(dialogue would go here)\n"
)

SAMPLE_DIGEST = (
    "- The Black-Briar family: owns the meadery and most of the guard's goodwill.\n"
    "- Keerava: runs the Bee and Barb, where most of the town drinks."
)

MULTILINE_COMMENT = (
    "{# Skills sit at 15 by default; anything meaningfully above that is a real signal\n"
    "   about what this person actually does. #}\n"
)
SINGLELINE_COMMENT = (
    "{# Skills sit at 15 by default; anything above that is a real signal. #}\n"
)

IF_CHAIN = (
    "{% if npc.oneHanded > 25 %}- One-handed: {{ npc.oneHanded }}\n"
    "{% endif %}{% if npc.twoHanded > 25 %}- Two-handed: {{ npc.twoHanded }}\n"
    "{% endif %}{% if npc.archery > 25 %}- Archery: {{ npc.archery }}\n"
    "{% endif %}{% if npc.speech > 25 %}- Speech: {{ npc.speech }}\n"
    "{% endif %}"
)

FACTIONS_BLOCK = (
    "## Factions\n"
    "{% for f in npc.faction %}{% if length(f) > 0 %}- {{ f }}\n"
    "{% endif %}{% endfor %}\n"
)

SKILL_CONDS = [
    "oneHanded", "twoHanded", "archery", "block", "sneak", "speech", "alchemy",
    "smithing", "destruction", "restoration", "illusion", "conjuration",
    "pickpocket", "lockpicking",
]


def skill_chain(n, plain_body=False):
    parts = []
    for i, skill in enumerate(SKILL_CONDS[:n]):
        body = "- Skill" if plain_body else f"- {skill.title()}: {{{{ npc.{skill} }}}}"
        parts.append(f"{{% if npc.{skill} > 25 %}}{body}\n{{% endif %}}")
    return "".join(parts)


CASES = {
    # G-series: does the template ENDING at a tag (no trailing text) eat the heading?
    "G_ends_at_endif": (
        SETUP + FACTIONS_BLOCK + "\n## Notable skills\n"
        + "{% if npc.speech > 100 %}- D\n{% endif %}"
    ),
    "G_ends_at_comment": (
        SETUP + FACTIONS_BLOCK + "\n## Notable skills\n" + MULTILINE_COMMENT
    ),
    "G_ends_at_endif_after_Y": (
        SETUP + FACTIONS_BLOCK + "\n## Notable skills\nY\n{% if npc.speech > 100 %}D\n{% endif %}"
    ),
    # H-series: the real mid-template shape - chain then MORE content after it
    "H4_chain_then_content": (
        SETUP + FACTIONS_BLOCK + "\n## Notable skills\n" + MULTILINE_COMMENT
        + skill_chain(4) + "\n## After\nZ\n"
    ),
    "H8_chain_then_content": (
        SETUP + FACTIONS_BLOCK + "\n## Notable skills\n" + MULTILINE_COMMENT
        + skill_chain(8) + "\n## After\nZ\n"
    ),
    "H14_chain_then_content": (
        SETUP + FACTIONS_BLOCK + "\n## Notable skills\n" + MULTILINE_COMMENT
        + skill_chain(14) + "\n## After\nZ\n"
    ),
    "H14_plain_bodies": (
        SETUP + FACTIONS_BLOCK + "\n## Notable skills\n" + MULTILINE_COMMENT
        + skill_chain(14, plain_body=True) + "\n## After\nZ\n"
    ),
    # D-series: the regional digest section, present and absent. Absent is the
    # one that can go wrong - the section must vanish WITHOUT taking the
    # heading that follows it.
    "D_digest_present": (DIGEST_BLOCK, {"regionDigest": SAMPLE_DIGEST}),
    "D_digest_absent": (DIGEST_BLOCK, {"regionDigest": ""}),
}

# Rendered by NAME from the installed prompts/, not from a snippet above.
LIVE_CASES = {
    "live_generate": ("bioforge_generate", {"regionDigest": SAMPLE_DIGEST}),
    "live_digest": ("bioforge_region_digest", {
        "regionName": "Riften",
        "holdName": "The Rift",
        "knownLocals": "- Keerava: runs the Bee and Barb.",
    }),
}


def render(content: str = None, template_name: str = None, **overrides):
    # actorUUID goes as a HEX STRING here. That is the opposite of what the DLL
    # injects (a JSON number) - the preview endpoint converts it for you, the
    # decorators do not. Same template, different caller.
    payload = {
        "actorUUID": TORG,
        "sourcePlugin": "Mara's Embrace.esp",
        "worldKnowledge": "",
        "relatedActors": "",
        "localActors": "",
        "regionDigest": "",
    }
    if content is not None:
        payload["content"] = content
    if template_name is not None:
        payload["templateName"] = template_name
    payload.update(overrides)
    req = urllib.request.Request(
        URL,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        return {"http_error": e.code, "body": e.read().decode("utf-8", "replace")[:400]}


def extract_text(result):
    """Return (ok, text) from whatever shape the preview endpoint answers with."""
    if not isinstance(result, dict):
        return False, json.dumps(result)[:400]
    for key in ("rendered", "renderedContent", "result", "content", "output", "text"):
        if key in result:
            v = result[key]
            if isinstance(v, str):
                return True, v
            if isinstance(v, dict) and "content" in v:
                return True, str(v["content"])
            if isinstance(v, dict) and "rendered" in v:
                return True, str(v["rendered"])
    return False, json.dumps(result)[:400]


def main():
    wanted = sys.argv[1:] or list(CASES) + list(LIVE_CASES)
    for name in wanted:
        if name in LIVE_CASES:
            template_name, overrides = LIVE_CASES[name]
            result = render(template_name=template_name, **overrides)
        else:
            case = CASES[name]
            content, overrides = case if isinstance(case, tuple) else (case, {})
            result = render(content=content, **overrides)

        ok, text = extract_text(result)
        print(f"=== {name} | ok={ok} | len={len(text)}")
        if not ok:
            print(f"    raw: {text}")
            continue

        headings = [ln for ln in text.splitlines() if ln.startswith("#")]
        print(f"    headings: {headings}")
        if name.startswith("D_") or name == "live_generate":
            # The heading below the digest must survive either way.
            print(f"    digest_heading={'## Who matters around here' in text} "
                  f"next_heading={'## Their own dialogue' in text}")
        print(f"    repr: {text!r}")
        print()


if __name__ == "__main__":
    main()
