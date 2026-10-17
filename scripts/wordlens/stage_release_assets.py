#!/usr/bin/env python3
"""Stage the committed Word Lens packs as GitHub release assets (used by .github/workflows/release.yml).

Extracts every pack listed in the archive's word-lens-manifest.json, the manifest itself and the
attribution notice (as word-lens-ATTRIBUTION.md) from lingua-data/wordlens/word-lens-packs.tar.xz
into --out, after checking that each pack matches the sha256 and size the manifest records and
still parses as a .lens file. Prints the number of staged files; exits non-zero on any mismatch
and on archive members the manifest does not list.

    python3 scripts/wordlens/stage_release_assets.py --out dist/wordlens
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lens_format  # noqa: E402
from pack_archive import ARCHIVE_NAME, ATTRIBUTION_NAME, MANIFEST_NAME, read_archive  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.abspath(os.path.join(HERE, "..", "..", "lingua-data", "wordlens"))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", default=DATA)
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)

    members = read_archive(os.path.join(args.data, ARCHIVE_NAME))
    if MANIFEST_NAME not in members or ATTRIBUTION_NAME not in members:
        print("archive lacks the manifest or the attribution", file=sys.stderr)
        return 1
    manifest = json.loads(members[MANIFEST_NAME].decode("utf-8"))
    if manifest.get("formatVersion") != lens_format.FORMAT_VERSION:
        print(f"manifest formatVersion {manifest.get('formatVersion')} != {lens_format.FORMAT_VERSION}", file=sys.stderr)
        return 1
    listed = {p["file"] for p in manifest["packs"]}
    extra = set(members) - listed - {MANIFEST_NAME, ATTRIBUTION_NAME}
    if extra:
        print(f"archive members not in the manifest: {', '.join(sorted(extra))}", file=sys.stderr)
        return 1
    os.makedirs(args.out, exist_ok=True)
    staged = 0
    for pack in manifest["packs"]:
        data = members.get(pack["file"])
        if data is None or len(data) != pack["bytes"] or hashlib.sha256(data).hexdigest() != pack["sha256"]:
            print(f"{pack['file']}: missing or does not match the manifest", file=sys.stderr)
            return 1
        try:
            lens_format.LensFile(data)
        except ValueError as exc:
            print(f"{pack['file']}: {exc}", file=sys.stderr)
            return 1
        with open(os.path.join(args.out, pack["file"]), "wb") as f:
            f.write(data)
        staged += 1
    with open(os.path.join(args.out, MANIFEST_NAME), "wb") as f:
        f.write(members[MANIFEST_NAME])
    with open(os.path.join(args.out, "word-lens-ATTRIBUTION.md"), "wb") as f:
        f.write(members[ATTRIBUTION_NAME])
    print(staged + 2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
