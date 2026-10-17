#!/usr/bin/env python3
"""Regenerate the Word Lens test fixtures in test/word_lens_format/fixtures/.

The JSON packs are small hand-made Readest-shaped packs that exercise every cleaning and folding
rule; the .lens.hex files next to them are what lens_format.build_lens() makes of them, as hex
text (no binary packs are committed). The C++ host test checks that the device converter
(LensBuilder) produces the same bytes, and test_wordlens.py checks that this script still
reproduces the committed files.

    python3 scripts/wordlens/make_fixtures.py [readest-data-wordlens-dir]

With a Readest data/wordlens directory it also refreshes gloss_cases.txt.
"""

from __future__ import annotations

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lens_format  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.abspath(os.path.join(HERE, "..", "..", "test", "word_lens_format", "fixtures"))


def mini_en_ru() -> dict:
    long_gloss = "；".join(f"очень-очень длинное толкование номер {i} с множеством слов" for i in range(6))
    return {
        "meta": {
            "source": "en",
            "target": "ru",
            "license": "CC-BY-SA-3.0",
            "attribution": "Fixture glosses (test only).",
            "metric": "frequency",
            "version": 1,
            "count": 0,
        },
        "entries": {
            "the": {"r": 1, "g": "определённый артикль"},
            "bloody": {"r": 1001, "g": "крова́вый；окрова́вленный"},
            "college": {"r": 1002, "g": "ПТУ；акаде́мия"},
            "french": {"r": 1003, "g": "францу́зский；] ]"},
            "worst": {"r": 4100, "g": "] ]；наиху́дшее"},
            "food": {"r": 2500, "g": "] [[пища；пи́щи]]"},
            "whisper": {"r": 6200, "g": "v. шепта́ть, шепну́ть；шёпот"},
            "kind": {"r": 900, "g": "добрый；сорт"},
            "kindly": {"r": 9000, "g": "любезно；добрый"},
            "empty": {"r": 5000, "g": "] ]；[[ ]]"},
            "nogloss": {"r": 5000},
            "norank": {"g": "без ранга"},
            "huge": {"r": 99999, "g": "огромный"},
            "neg": {"r": -5, "g": "отрицательный"},
            "verbose": {"r": 15000, "g": long_gloss},
            "paren": {"r": 7000, "g": "(разг.) болтовня；трёп"},
            "zebra": {"r": 23999, "g": "зебра"},
            "aardvark": {"r": 24000, "g": "трубкозуб"},
            "don't": {"r": 1500, "g": "не делай"},
            "naïve": {"r": 8000, "g": "наивный"},
            "x" * 300: {"r": 5000, "g": "слишком длинный ключ"},
        },
        "inflections": {
            "whispered": "whisper",
            "whispers": "whisper",
            "whispering": "whisper",
            "kinder": "kind",
            "foods": "food",
            "orphaned": "nowhere",
            "emptied": "empty",
            "bloodier": "bloody",
            "worse": "worst",
            "zebras": "zebra",
        },
    }


def mini_en_en() -> dict:
    return {
        "meta": {"source": "en", "target": "en", "metric": "frq", "version": 1, "count": 0},
        "entries": {
            "the": {"r": 1, "g": "See Thee; definite article. A word placed before nouns to limit or"},
            "atypically": {"r": 34637, "g": "in a manner that is not typical"},
            "runnel": {"r": 34639, "g": "run"},
            "thick": {"r": 3000, "g": "having a large distance between surfaces"},
            "thickly": {"r": 12000, "g": "in a thick manner"},
            "lazy": {"r": 5000, "g": "idle, slothful"},
            "lazily": {"r": 11000, "g": "in an idle manner"},
            "shy": {"r": 4500, "g": "timid; bashful"},
            "shyly": {"r": 13000, "g": "in a timid way"},
            "hard": {"r": 600, "g": "difficult"},
            "hardly": {"r": 9000, "g": "barely, scarcely"},
        },
        "inflections": {"thicker": "thick", "runnels": "runnel"},
    }


def to_hex(data: bytes) -> str:
    """64 hex digits per line."""
    h = data.hex()
    return "".join(h[i : i + 64] + "\n" for i in range(0, len(h), 64))


def write(name: str, pack: dict) -> None:
    os.makedirs(FIXTURES, exist_ok=True)
    raw = json.dumps(pack, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    with open(os.path.join(FIXTURES, name + ".json"), "wb") as f:
        f.write(raw)
    image, _ = lens_format.build_lens(json.loads(raw.decode("utf-8")))
    with open(os.path.join(FIXTURES, name + ".lens.hex"), "w", encoding="ascii") as f:
        f.write(to_hex(image))


FIXTURE_PACKS = {"mini-en-ru": mini_en_ru, "mini-en-en": mini_en_en}


def write_gloss_cases(readest_dir: str, per_pack: int = 120) -> None:
    """Sample real glosses from Readest packs into gloss_cases.txt: `<mono> <hex raw> <hex clean>`
    per line, so the C++ cleaner is checked against this one on real data, not just hand-made
    edge cases."""
    lines = []
    for pair in ["en-ru", "en-en", "de-en", "en-zh", "en-ar", "it-en", "ru-en", "en-de", "pt-en", "en-vi"]:
        path = os.path.join(readest_dir, pair + ".json")
        if not os.path.exists(path):
            continue
        with open(path, "r", encoding="utf-8") as f:
            pack = json.load(f)
        mono = pack["meta"]["source"] == pack["meta"]["target"]
        values = [str(v.get("g", "")) for v in pack["entries"].values()]
        # Every gloss with markup, brackets or marks first, then an even spread of the rest.
        odd = [g for g in values if any(c in g for c in "[](){}|/;") or any(0x300 <= ord(c) <= 0x36F for c in g)]
        step = max(1, len(values) // per_pack)
        chosen = odd[: per_pack // 2] + values[::step][: per_pack // 2]
        for raw in chosen:
            clean = lens_format.clean_gloss(raw, mono)
            lines.append(f"{int(mono)} {raw.encode().hex()} {clean.encode().hex() or '-'}")
    with open(os.path.join(FIXTURES, "gloss_cases.txt"), "w", encoding="ascii") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote gloss_cases.txt ({len(lines)} cases)", file=sys.stderr)


def main() -> int:
    for name, make in FIXTURE_PACKS.items():
        write(name, make())
        print(f"wrote {name}.json / {name}.lens.hex", file=sys.stderr)
    if len(sys.argv) > 1:
        write_gloss_cases(sys.argv[1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
