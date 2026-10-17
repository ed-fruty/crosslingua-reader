#!/usr/bin/env python3
"""Derive the English -> Ukrainian Word Lens pack from Readest's English -> Russian pack.

The first cleaned sense of every Russian gloss is machine-translated into Ukrainian through
Google's free translateHtml endpoint -- the one the firmware's "Google (Free) - HTML" engine
calls -- in batches of up to 100 glosses, rate-limited. Results live in en-uk.translations.tsv next to this
script (`russian<TAB>ukrainian`, sorted, one line per distinct Russian gloss; an empty
Ukrainian side records a failed translation). The cache is committed, so a rebuild is offline
and deterministic and only Russian glosses that are new (or failed last time) hit the network.

Normally driven by update_packs.py; standalone use fills the cache:

    python3 scripts/wordlens/make_en_uk.py --en-ru path/to/en-ru.json
"""

from __future__ import annotations

import argparse
import html
import json
import os
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Callable

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lens_format import SENSE_JOINER, clean_gloss, load_pack_json  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CACHE = os.path.join(HERE, "en-uk.translations.tsv")
# Google's translateHtml endpoint with the public web key, i.e. the firmware's "Google (Free) - HTML"
# engine (ParagraphTranslator::translateGoogleHtml). It takes an array of texts per request.
ENDPOINT = "https://translate-pa.googleapis.com/v1/translateHtml"
API_KEY = "AIzaSyATBXajvzQLTDHEQbcpq0Ihe0vWDHmO520"
BATCH_CHARS = 2400
BATCH_ITEMS = 100
REQUEST_INTERVAL_S = 1.0

EN_UK_ATTRIBUTION = (
    "Glosses: machine-translated (Google Translate) from the Readest en-ru Word Lens pack, whose "
    "glosses come from WikDict (CC-BY-SA-3.0), derived from DBnary/Wiktionary. Frequency: "
    "hermitdave/FrequencyWords from OpenSubtitles/OPUS (CC-BY-SA-4.0). License: CC-BY-SA-3.0."
)

# Translates a batch of Russian strings; returns one Ukrainian string per input ("" = failed).
Translator = Callable[[list[str]], list[str]]


@dataclass
class UkStats:
    unique_glosses: int = 0
    reused: int = 0
    newly_translated: int = 0
    failed: int = 0
    dropped_entries: int = 0
    failed_samples: list[str] = field(default_factory=list)


def load_cache(path: str) -> dict[str, str]:
    cache: dict[str, str] = {}
    if not os.path.exists(path):
        return cache
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if "\t" not in line:
                continue
            ru, uk = line.split("\t", 1)
            if ru:
                cache[ru] = uk
    return cache


def save_cache(path: str, cache: dict[str, str]) -> None:
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        for ru in sorted(cache):
            f.write(f"{ru}\t{cache[ru]}\n")
    os.replace(tmp, path)


def _translate_html_request(lines: list[str]) -> list[str] | None:
    """One POST to Google's translateHtml endpoint: an array of texts in, one translation per text
    out, in order. Inputs are HTML-escaped (the endpoint parses markup) and outputs unescaped."""
    body = json.dumps([[[html.escape(line, quote=False) for line in lines], "ru", "uk"], "wt_lib"]).encode("utf-8")
    req = urllib.request.Request(
        ENDPOINT,
        data=body,
        headers={
            "Content-Type": "application/json+protobuf",
            "X-Goog-Api-Key": API_KEY,
            "User-Agent": "CrossLingua-WordLens-builder/1.0",
        },
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        payload = json.loads(resp.read().decode("utf-8"))
    out = payload[0] if payload and isinstance(payload[0], list) else None
    if out is None or len(out) != len(lines):
        return None
    return [html.unescape(str(s)).strip() for s in out]


def google_translator(lines: list[str]) -> list[str]:
    """Batch with backoff; per-line fallback when the batch answer does not line up."""
    backoff = 5.0
    for _ in range(8):
        try:
            result = _translate_html_request(lines)
            break
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, OSError) as exc:
            print(f"en-uk: request failed ({exc}); retrying in {backoff:.0f}s", file=sys.stderr)
            time.sleep(backoff)
            backoff = min(backoff * 2, 300.0)
    else:
        return [""] * len(lines)
    if result is not None:
        return result
    out = []
    for line in lines:
        time.sleep(REQUEST_INTERVAL_S)
        try:
            single = _translate_html_request([line])
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, OSError):
            single = None
        out.append(single[0] if single else "")
    return out


def first_sense_ru(raw: str) -> str:
    # Only the first sense is translated: it is what the strip shows, and a single word or phrase
    # translates far more reliably than a list of near-synonyms.
    return clean_gloss(raw, monolingual=False).split(SENSE_JOINER, 1)[0]


def fill_cache(
    glosses: list[str],
    cache: dict[str, str],
    translator: Translator | None,
    stats: UkStats,
    on_batch: Callable[[], None] | None = None,
    interval_s: float = REQUEST_INTERVAL_S,
) -> None:
    """Translate every gloss that is not in the cache yet (a cached empty result is retried)."""
    todo = [g for g in glosses if not cache.get(g)]
    stats.reused = len(glosses) - len(todo)
    if translator is None or not todo:
        return
    print(f"en-uk: {len(todo)} glosses to translate", file=sys.stderr)
    i = 0
    while i < len(todo):
        batch: list[str] = []
        chars = 0
        while i + len(batch) < len(todo) and len(batch) < BATCH_ITEMS:
            nxt = todo[i + len(batch)]
            if batch and chars + len(nxt) + 1 > BATCH_CHARS:
                break
            batch.append(nxt)
            chars += len(nxt) + 1
        for ru, uk in zip(batch, translator(batch)):
            cache[ru] = uk
            if uk:
                stats.newly_translated += 1
        i += len(batch)
        if on_batch:
            on_batch()
        if i < len(todo) and interval_s:
            time.sleep(interval_s)


def derive_en_uk(
    en_ru: dict,
    cache_path: str = DEFAULT_CACHE,
    translator: Translator | None = google_translator,
    stats: UkStats | None = None,
    persist: bool = True,
) -> dict:
    """Return an en-uk pack dict shaped like a Readest pack. translator=None means offline."""
    stats = stats if stats is not None else UkStats()
    cleaned = {key: first_sense_ru(str(value.get("g", ""))) for key, value in en_ru["entries"].items()}
    unique = sorted({g for g in cleaned.values() if g})
    stats.unique_glosses = len(unique)
    cache = load_cache(cache_path)
    fill_cache(unique, cache, translator, stats, on_batch=(lambda: save_cache(cache_path, cache)) if persist else None)
    if persist and translator is not None:
        save_cache(cache_path, cache)
    stats.failed = sum(1 for g in unique if not cache.get(g))
    stats.failed_samples = [g for g in unique if not cache.get(g)][:10]
    entries = {}
    for key, value in en_ru["entries"].items():
        uk = cache.get(cleaned[key], "") if cleaned[key] else ""
        if not uk:
            stats.dropped_entries += 1
        entries[key] = {"r": value["r"], "g": uk}
    meta = dict(en_ru.get("meta", {}))
    meta["target"] = "uk"
    meta["attribution"] = EN_UK_ATTRIBUTION
    meta.pop("license", None)
    meta["count"] = sum(1 for v in entries.values() if v["g"])
    return {"meta": meta, "entries": entries, "inflections": en_ru.get("inflections", {})}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--en-ru", required=True, help="Readest en-ru.json")
    ap.add_argument("--cache", default=DEFAULT_CACHE)
    ap.add_argument("--offline", action="store_true", help="never call the translation service")
    ap.add_argument("--out", help="also write the derived pack as JSON")
    args = ap.parse_args()
    stats = UkStats()
    pack = derive_en_uk(load_pack_json(args.en_ru), args.cache, None if args.offline else google_translator, stats)
    print(
        f"en-uk: {stats.unique_glosses} glosses: {stats.reused} reused, {stats.newly_translated} newly translated, "
        f"{stats.failed} failed; {stats.dropped_entries} entries dropped",
        file=sys.stderr,
    )
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(pack, f, ensure_ascii=False, separators=(",", ":"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
