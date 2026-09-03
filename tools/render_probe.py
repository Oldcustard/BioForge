"""Probe SkyrimNet's render-template-preview to isolate the vanishing
`## Notable skills` heading in bioforge_generate.prompt.

Usage: python render_probe.py [case ...]   (default: all cases)
"""
import json
import sys
import urllib.error
import urllib.request

URL = "http://127.0.0.1:8080/prompts?api=render-template-preview"
TORG = "0CDD2B350A026B6D"  # hex-string form; decimal string renders FFFF...

SETUP = "{% set npc = decnpc(actorUUID) %}\n"

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
}


def render(content: str):
    payload = {
        "content": content,
        "actorUUID": TORG,
        "sourcePlugin": "Mara's Embrace.esp",
        "worldKnowledge": "",
        "relatedActors": "",
    }
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
    wanted = sys.argv[1:] or list(CASES)
    for name in wanted:
        content = CASES[name]
        result = render(content)
        ok, text = extract_text(result)
        print(f"=== {name} | ok={ok} | len={len(text)}")
        if not ok:
            print(f"    raw: {text}")
            continue
        has_heading = "## Notable skills" in text
        has_factions = "## Factions" in text
        print(f"    factions_heading={has_factions} skills_heading={has_heading}")
        print(f"    repr: {text!r}")
        print()


if __name__ == "__main__":
    main()
