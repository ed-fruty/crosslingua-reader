#!/usr/bin/env python3
"""Refresh the Word Lens packs in lingua-data/wordlens/ from the Readest CDN.

The packs, their manifest and ATTRIBUTION.md live in one committed archive,
lingua-data/wordlens/word-lens-packs.tar.xz (see pack_archive.py); ATTRIBUTION.md is also kept
next to it as plain text.

Idempotent: a pack is re-downloaded and re-converted only when Readest's JSON changed (its
sha256 in the Readest manifest), when our format/data version changed, or when our .lens is
missing or does not match our manifest. Conversion is deterministic, so identical input gives
byte-identical .lens files and an unchanged archive.

en-uk is derived from en-ru (make_en_uk.py): Russian glosses already in the committed
translation cache are reused; only new ones (and, with --retry-failed, previously failed ones)
are sent to the translation service. Entries whose gloss could not be translated are dropped.

    python3 scripts/wordlens/update_packs.py                     # refresh everything that changed
    python3 scripts/wordlens/update_packs.py --dry-run           # report what would change
    python3 scripts/wordlens/update_packs.py --pairs en-ru,en-uk # limit to some pairs
    python3 scripts/wordlens/update_packs.py --readest-dir ~/readest/apps/readest-app/data/wordlens
    python3 scripts/wordlens/update_packs.py --offline           # never call the translation service

Exits non-zero when a download does not match its sha256 or a built pack fails verification.
Commits are left to you; the script prints the paths to `git add`.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lens_format  # noqa: E402
import make_en_uk  # noqa: E402
from pack_archive import ARCHIVE_NAME, ATTRIBUTION_NAME, MANIFEST_NAME, read_archive, write_archive  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_OUT = os.path.join(REPO, "lingua-data", "wordlens")
READEST_CDN = "https://cdn.readest.com/wordlens"

# Data release number of the generated packs. Bump when the packs change for a reason the
# Readest sha256 cannot see (cleaning rules, en-uk derivation), so every pack is rebuilt.
DATA_VERSION = 1

# Source languages the firmware's Word Lens tokenizer handles: space-delimited Latin and Cyrillic
# scripts. zh/ja/ko/th need a word segmenter, and Readest ships no vi/hu/ar source packs.
SUPPORTED_SOURCES = {"en", "de", "es", "fr", "it", "pt", "ru"}
DERIVED = {"en-uk": "en-ru"}


class VerificationError(Exception):
    pass


def sha256_of(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def pack_file_name(pair: str) -> str:
    return f"word-lens-{pair}.lens"


class ReadestSource:
    """Readest's manifest and JSON packs, from a local checkout or from the CDN."""

    def __init__(self, readest_dir: str | None):
        self.dir = readest_dir
        self.manifest = json.loads(self._read("manifest.json", None))
        self.by_pair = {p["pair"]: p for p in self.manifest["packs"]}

    def _read(self, name: str, sha: str | None) -> bytes:
        if self.dir:
            with open(os.path.join(self.dir, name), "rb") as f:
                return f.read()
        # Same URL scheme as Readest's app (glossPacks.ts): pack URLs carry a sha8 cache-buster.
        url = f"{READEST_CDN}/{name}" + (f"?v={sha[:8]}" if sha else "")
        req = urllib.request.Request(url, headers={"User-Agent": "CrossLingua-WordLens-builder/1.0"})
        with urllib.request.urlopen(req, timeout=120) as resp:
            return resp.read()

    def pack_json(self, pair: str) -> tuple[dict, bytes]:
        entry = self.by_pair[pair]
        print(f"fetch {entry['file']}", file=sys.stderr)
        data = self._read(entry["file"], entry["sha256"])
        if sha256_of(data) != entry["sha256"] or len(data) != entry["bytes"]:
            raise VerificationError(f"{pair}: download does not match the Readest manifest sha256/bytes")
        return json.loads(data.decode("utf-8"), object_pairs_hook=_no_duplicates(pair)), data


def _no_duplicates(pair: str):
    def hook(pairs):
        seen = set()
        for k, _ in pairs:
            if k in seen:
                raise VerificationError(f"{pair}: duplicate JSON key {k!r}")
            seen.add(k)
        return dict(pairs)

    return hook


def load_manifest(members: dict[str, bytes]) -> dict:
    raw = members.get(MANIFEST_NAME)
    return json.loads(raw.decode("utf-8")) if raw is not None else {"packs": []}


def read_bytes(path: str) -> bytes | None:
    if not os.path.exists(path):
        return None
    with open(path, "rb") as f:
        return f.read()


def file_sha(path: str) -> str | None:
    data = read_bytes(path)
    return None if data is None else sha256_of(data)


def verify_image(pair: str, image: bytes, pack: dict) -> lens_format.LensFile:
    """Every kept headword must resolve to its own lemma entry."""
    try:
        lens = lens_format.LensFile(image)
    except ValueError as exc:
        raise VerificationError(f"{pair}: {exc}") from exc
    mono = pack["meta"]["source"] == pack["meta"]["target"]
    for key, value in pack["entries"].items():
        if not lens_format.clean_gloss(str(value.get("g", "")), mono):
            continue
        hit = lens.lookup(key)
        if hit is None or hit[1] != key:
            raise VerificationError(f"{pair}: headword {key!r} does not resolve")
    return lens


def key_set(image: bytes | None) -> set[str]:
    if not image:
        return set()
    return {k for k, _, _, _ in lens_format.LensFile(image).iter_records()}


def manifest_entry(pair, meta, image, stats, extra):
    entry = {
        "pair": pair,
        "source": meta["source"],
        "target": meta["target"],
        "file": pack_file_name(pair),
        "bytes": len(image),
        "sha256": sha256_of(image),
        "entries": stats.records,
        "headwords": stats.headwords,
        "version": DATA_VERSION,
        "contentHash": f"{lens_format.LensFile(image).content_hash:08x}",
    }
    entry.update(extra)
    return entry


def attribution_text(packs: list[dict], attributions: dict[str, str]) -> str:
    lines = [
        "# Word Lens data attributions",
        "",
        "The `.lens` packs in this directory are converted from Readest's Word Lens JSON packs",
        "(https://github.com/readest/readest, `apps/readest-app/data/wordlens`, served from",
        f"{READEST_CDN}) by `scripts/wordlens/update_packs.py`. Glosses are cleaned (first",
        "senses only, markup, part-of-speech tags, stress marks and Arabic vowel signs removed).",
        "",
        "Sources and licences (CC-BY-SA data stays CC-BY-SA in the converted packs):",
        "",
        "- WikDict (bilingual glosses) - CC-BY-SA 3.0, derived from DBnary / Wiktionary. https://www.wikdict.com/",
        "- kaikki.org wiktextract dump of the English Wiktionary (en-vi, en-hu, en-ar glosses) - CC-BY-SA 4.0.",
        "  https://kaikki.org/dictionary/rawdata.html",
        "- FrequencyWords (difficulty ranks, OpenSubtitles/OPUS) - CC-BY-SA 4.0. https://github.com/hermitdave/FrequencyWords",
        "- ECDICT (en-zh and en-en glosses, ranks, English inflections) - MIT. https://github.com/skywind3000/ECDICT",
        "- WordNet (en-en synonyms and hypernyms) - WordNet 3.0 licence. https://wordnet.princeton.edu/",
        "- lemmatization-lists (source-language inflections) - https://github.com/michmech/lemmatization-lists",
        "",
        "`word-lens-en-uk.lens` is derived by CrossLingua: its glosses are the en-ru pack's Russian",
        "glosses machine-translated into Ukrainian (Google Translate), with en-ru's ranks and",
        "inflections. It inherits en-ru's CC-BY-SA licence.",
        "",
        "Each pack also carries its own attribution string (shown on the device):",
        "",
    ]
    for p in packs:
        lines.append(f"- `{p['file']}`: {attributions.get(p['pair'], lens_format.DEFAULT_ATTRIBUTION)}")
    lines.append("")
    return "\n".join(lines)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--readest-dir", help="use a local Readest data/wordlens checkout instead of the CDN")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--en-uk-cache", default=make_en_uk.DEFAULT_CACHE)
    ap.add_argument("--pairs", help="comma-separated pairs to process (default: all supported)")
    ap.add_argument("--dry-run", action="store_true", help="report changes without writing or translating")
    ap.add_argument("--offline", action="store_true", help="never call the translation service")
    ap.add_argument("--retry-failed", action="store_true", help="re-derive en-uk to retry failed translations")
    ap.add_argument("--force", action="store_true", help="rebuild every selected pack")
    ap.add_argument("--samples", type=int, default=0, help="print N sample glosses per rebuilt pack")
    args = ap.parse_args(argv)

    try:
        return run(args)
    except VerificationError as exc:
        print(f"verification failed: {exc}", file=sys.stderr)
        return 2


def run(args) -> int:
    out_dir = args.out
    archive_path = os.path.join(out_dir, ARCHIVE_NAME)
    members = read_archive(archive_path)
    source = ReadestSource(args.readest_dir)
    old = load_manifest(members)
    old_by_pair = {p["pair"]: p for p in old.get("packs", [])}

    wanted = None if not args.pairs else {p.strip() for p in args.pairs.split(",") if p.strip()}
    upstream_pairs = sorted(p for p, e in source.by_pair.items() if e["source"] in SUPPORTED_SOURCES)
    all_pairs = sorted(set(upstream_pairs) | {d for d, base in DERIVED.items() if base in source.by_pair})
    selected = [p for p in all_pairs if wanted is None or p in wanted]
    if wanted:
        unknown = wanted - set(all_pairs)
        if unknown:
            print(f"unknown pairs: {', '.join(sorted(unknown))}", file=sys.stderr)
            return 2

    def up_to_date(pair: str, readest_sha: str | None, extra_key: str | None = None, extra_val=None) -> bool:
        prev = old_by_pair.get(pair)
        if args.force or not prev:
            return False
        if prev.get("version") != DATA_VERSION or old.get("formatVersion") != lens_format.FORMAT_VERSION:
            return False
        if readest_sha and prev.get("readest", {}).get("sha256") != readest_sha:
            return False
        if extra_key and prev.get(extra_key) != extra_val:
            return False
        data = members.get(prev["file"])
        return data is not None and sha256_of(data) == prev["sha256"]

    new_entries: dict[str, dict] = {}
    images: dict[str, bytes] = {}
    attributions: dict[str, str] = {}
    changed: list[str] = []
    loaded_json: dict[str, dict] = {}

    def rebuild(pair: str, pack: dict, extra: dict):
        image, stats = lens_format.build_lens(pack)
        lens = verify_image(pair, image, pack)
        attributions[pair] = lens.attribution
        entry = manifest_entry(pair, pack["meta"], image, stats, extra)
        prev = old_by_pair.get(pair)
        if prev and prev.get("sha256") == entry["sha256"]:
            new_entries[pair] = entry
            return
        images[pair] = image
        new_entries[pair] = entry
        changed.append(pair)
        old_image = members.get(pack_file_name(pair))
        before, after = key_set(old_image), key_set(image)
        delta = len(image) - (len(old_image) if old_image else 0)
        print(
            f"{pair}: {stats.records} records ({stats.headwords} headwords), {len(image)} bytes ({delta:+d}); "
            f"+{len(after - before)} / -{len(before - after)} keys",
            file=sys.stderr,
        )
        if args.samples:
            keys = [k for k, v in pack["entries"].items() if 4000 <= int(v.get("r", 0)) <= 20000]
            step = max(1, len(keys) // args.samples)
            for k in keys[::step][: args.samples]:
                hit = lens.lookup(k)
                if hit:
                    print(f"    {k:<16} r={hit[0]:<6} {lens_format.display_gloss(hit[2])}", file=sys.stderr)

    for pair in selected:
        if pair in DERIVED:
            continue
        readest = source.by_pair[pair]
        if up_to_date(pair, readest["sha256"]):
            new_entries[pair] = old_by_pair[pair]
            continue
        pack, _ = source.pack_json(pair)
        loaded_json[pair] = pack
        rebuild(pair, pack, {"readest": {k: readest[k] for k in ("file", "bytes", "sha256")}})

    uk_stats = None
    for pair, base in DERIVED.items():
        if pair not in selected:
            continue
        base_sha = source.by_pair[base]["sha256"]
        cache_sha = file_sha(args.en_uk_cache)
        if not args.retry_failed and up_to_date(pair, None, "derivedFrom", {"pair": base, "sha256": base_sha}) and \
                old_by_pair[pair].get("translationCacheSha256") == cache_sha:
            new_entries[pair] = old_by_pair[pair]
            continue
        base_pack = loaded_json.get(base) or source.pack_json(base)[0]
        translator = None if (args.offline or args.dry_run) else make_en_uk.google_translator
        uk_stats = make_en_uk.UkStats()
        uk = make_en_uk.derive_en_uk(base_pack, args.en_uk_cache, translator, uk_stats, persist=not args.dry_run)
        rebuild(
            pair,
            uk,
            {
                "derivedFrom": {"pair": base, "sha256": base_sha},
                "translationCacheSha256": file_sha(args.en_uk_cache),
            },
        )
        print(
            f"{pair}: {uk_stats.unique_glosses} Russian glosses: {uk_stats.reused} reused from cache, "
            f"{uk_stats.newly_translated} newly translated, {uk_stats.failed} failed; "
            f"{uk_stats.dropped_entries} entries dropped",
            file=sys.stderr,
        )

    # Packs outside the selection keep their previous manifest entries.
    for pair, prev in old_by_pair.items():
        if pair not in new_entries:
            new_entries[pair] = prev
    packs = sorted(new_entries.values(), key=lambda p: p["pair"])
    for p in packs:
        if p["pair"] not in attributions and p["file"] in members:
            attributions[p["pair"]] = lens_format.LensFile(members[p["file"]]).attribution
    manifest = {
        "schemaVersion": 1,
        "formatVersion": lens_format.FORMAT_VERSION,
        "dataVersion": DATA_VERSION,
        "readestCdn": READEST_CDN,
        "packs": packs,
    }

    if not changed:
        print("all packs up to date", file=sys.stderr)
    if args.dry_run:
        print(f"dry run: {len(changed)} pack(s) would change: {', '.join(changed) or '-'}", file=sys.stderr)
        return 0

    os.makedirs(out_dir, exist_ok=True)
    manifest_text = json.dumps(manifest, ensure_ascii=False, indent=1) + "\n"
    attribution = attribution_text(packs, attributions)
    new_members: dict[str, bytes] = {MANIFEST_NAME: manifest_text.encode("utf-8"),
                                     ATTRIBUTION_NAME: attribution.encode("utf-8")}
    for p in packs:
        image = images.get(p["pair"]) or members.get(p["file"])
        if image is None:
            raise VerificationError(f"{p['pair']}: no pack image to archive")
        new_members[p["file"]] = image
    archive_changed = write_archive(archive_path, new_members)
    attribution_path = os.path.join(out_dir, ATTRIBUTION_NAME)
    if read_bytes(attribution_path) != attribution.encode("utf-8"):
        with open(attribution_path, "w", encoding="utf-8") as f:
            f.write(attribution)
        archive_changed = True

    rel = os.path.relpath(out_dir, REPO)
    total = sum(p["bytes"] for p in packs)
    print(f"{len(packs)} packs, {total / 1e6:.1f} MB unpacked, {os.path.getsize(archive_path) / 1e6:.1f} MB "
          f"archive in {rel}", file=sys.stderr)
    if archive_changed:
        print("suggested: git add " + rel + " " + os.path.relpath(args.en_uk_cache, REPO))
    return 0


if __name__ == "__main__":
    sys.exit(main())
