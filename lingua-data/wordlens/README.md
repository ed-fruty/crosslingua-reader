# Word Lens packs

Gloss packs for the Lingua **Word Lens** display mode (see `docs/lingua.md`). They are
data, not firmware: nothing here is built into the firmware image.

| File | What |
| --- | --- |
| `word-lens-packs.tar.xz` | every `word-lens-<source>-<target>.lens` pack, `word-lens-manifest.json` and `ATTRIBUTION.md` |
| `ATTRIBUTION.md` | the same attribution notice, as plain text |

The packs are committed as one reproducible archive instead of loose binaries
(`scripts/wordlens/pack_archive.py`: sorted members, zero timestamps and owners, xz preset 9e),
so an unchanged pack set leaves the archive byte-identical. Every CrossLingua release extracts
it and attaches the packs, the manifest and `word-lens-ATTRIBUTION.md` as individual assets
(`.github/workflows/release.yml`); devices download them from
`https://github.com/ed-fruty/crosslingua-reader/releases/latest/download/`.

To look inside: `tar -tJf word-lens-packs.tar.xz`, or `tar -xJf word-lens-packs.tar.xz -C /tmp/packs`.

## Updating

Everything here is generated; do not edit it by hand.

```bash
python3 scripts/wordlens/update_packs.py --dry-run   # report what Readest changed
python3 scripts/wordlens/update_packs.py             # rebuild changed packs, manifest, ATTRIBUTION.md
python3 scripts/wordlens/update_packs.py --pairs en-ru,en-uk
```

The script fetches Readest's packs from `https://cdn.readest.com/wordlens/`, checks their
sha256, converts only those whose source changed (byte-identical output for identical
input), and rebuilds `en-uk` from `en-ru`. English -> Ukrainian glosses are the
English -> Russian ones machine-translated into Ukrainian; the translations are cached in
`scripts/wordlens/en-uk.translations.tsv`, so only Russian glosses not seen before are sent
to the translation service (`--offline` sends none, `--retry-failed` retries failures).
It prints a per-pack summary and the paths to `git add`; committing is up to you. The next
release publishes the new packs.

Tests (offline): `cd scripts/wordlens && python3 -m unittest -v test_wordlens`.
