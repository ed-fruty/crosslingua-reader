"""Offline tests for the Word Lens pack tooling (run: python3 -m unittest -v test_wordlens)."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lens_format  # noqa: E402
import make_en_uk  # noqa: E402
import make_fixtures  # noqa: E402
import pack_archive  # noqa: E402
import stage_release_assets  # noqa: E402
import update_packs  # noqa: E402


def _quiet():
    return contextlib.redirect_stderr(io.StringIO())


class ConversionTest(unittest.TestCase):
    def test_committed_fixtures_are_reproduced(self):
        for name, make in make_fixtures.FIXTURE_PACKS.items():
            path = os.path.join(make_fixtures.FIXTURES, name + ".lens.hex")
            with open(path, "r", encoding="ascii") as f:
                committed = bytes.fromhex(f.read().replace("\n", ""))
            raw = json.dumps(make(), ensure_ascii=False, separators=(",", ":"))
            image, _ = lens_format.build_lens(json.loads(raw))
            self.assertEqual(image, committed, f"{name}.lens is stale; run make_fixtures.py")

    def test_conversion_is_deterministic(self):
        pack = make_fixtures.mini_en_ru()
        a, _ = lens_format.build_lens(json.loads(json.dumps(pack)))
        b, _ = lens_format.build_lens(json.loads(json.dumps(pack)))
        self.assertEqual(a, b)

    def test_inflections_fold_to_their_lemma(self):
        image, stats = lens_format.build_lens(make_fixtures.mini_en_ru())
        lens = lens_format.LensFile(image)
        rank, lemma, gloss = lens.lookup("whispering")
        self.assertEqual((rank, lemma), (6200, "whisper"))
        self.assertEqual(gloss, "шептать; шёпот")
        self.assertIsNone(lens.lookup("orphaned"))
        self.assertIsNone(lens.lookup("empty"))
        self.assertEqual(stats.dropped_empty_gloss, 2)

    def test_gloss_cleaning(self):
        self.assertEqual(lens_format.clean_gloss("крова́вый；окрова́вленный", False), "кровавый; окровавленный")
        self.assertEqual(lens_format.display_gloss("in a manner that is not typical"), "in a manner that is not…")
        self.assertEqual(lens_format.clean_gloss("] ]；[[ ]]", False), "")


class ArchiveTest(unittest.TestCase):
    def test_archive_is_reproducible(self):
        members = {"b.lens": b"\x00\x01" * 100, "a.json": b"{}", "ATTRIBUTION.md": b"text"}
        a = pack_archive.archive_bytes(members)
        b = pack_archive.archive_bytes(dict(reversed(list(members.items()))))
        self.assertEqual(a, b)
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "x.tar.xz")
            self.assertTrue(pack_archive.write_archive(path, members))
            self.assertFalse(pack_archive.write_archive(path, members))
            self.assertEqual(pack_archive.read_archive(path), members)


class FakeTranslator:
    def __init__(self, table, fail=()):
        self.table = table
        self.fail = set(fail)
        self.calls: list[str] = []

    def __call__(self, lines):
        self.calls.extend(lines)
        return ["" if ru in self.fail else self.table.get(ru, "uk:" + ru) for ru in lines]


def en_ru_pack():
    return {
        "meta": {"source": "en", "target": "ru", "license": "CC-BY-SA-3.0", "attribution": "x"},
        "entries": {
            "bloody": {"r": 1001, "g": "крова́вый；окрова́вленный"},
            "gory": {"r": 9000, "g": "кровавый"},
            "whisper": {"r": 6200, "g": "шепта́ть"},
            "mumble": {"r": 7000, "g": "бормота́ть"},
        },
        "inflections": {"whispered": "whisper"},
    }


class EnUkCacheTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.cache = os.path.join(self.dir, "cache.tsv")

    def tearDown(self):
        shutil.rmtree(self.dir)

    def test_only_missing_glosses_are_translated_and_failures_dropped(self):
        make_en_uk.save_cache(self.cache, {"кровавый": "кривавий"})
        translator = FakeTranslator({"шептать": "шепотіти"}, fail={"бормотать"})
        stats = make_en_uk.UkStats()
        pack = make_en_uk.derive_en_uk(en_ru_pack(), self.cache, translator, stats)
        # "кровавый" (bloody and gory share it) came from the cache; only two glosses were sent.
        self.assertEqual(sorted(translator.calls), ["бормотать", "шептать"])
        self.assertEqual((stats.unique_glosses, stats.reused, stats.newly_translated, stats.failed), (3, 1, 1, 1))
        self.assertEqual(stats.dropped_entries, 1)
        self.assertEqual(pack["entries"]["bloody"]["g"], "кривавий")
        self.assertEqual(pack["entries"]["mumble"]["g"], "")
        self.assertEqual(pack["inflections"], {"whispered": "whisper"})
        self.assertEqual(pack["meta"]["target"], "uk")

        # A re-run reuses everything that succeeded and retries only the failure.
        retry = FakeTranslator({"бормотать": "бурмотіти"})
        stats = make_en_uk.UkStats()
        pack = make_en_uk.derive_en_uk(en_ru_pack(), self.cache, retry, stats)
        self.assertEqual(retry.calls, ["бормотать"])
        self.assertEqual(stats.reused, 2)
        self.assertEqual(pack["entries"]["mumble"]["g"], "бурмотіти")

    def test_offline_never_calls_out_and_the_cache_is_sorted(self):
        make_en_uk.save_cache(self.cache, {"шептать": "шепотіти", "кровавый": "кривавий"})
        stats = make_en_uk.UkStats()
        pack = make_en_uk.derive_en_uk(en_ru_pack(), self.cache, None, stats)
        self.assertEqual(stats.newly_translated, 0)
        self.assertEqual(stats.failed, 1)
        self.assertEqual(pack["entries"]["whisper"]["g"], "шепотіти")
        with open(self.cache, encoding="utf-8") as f:
            lines = f.read().splitlines()
        self.assertEqual(lines, sorted(lines))


class UpdatePacksTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.readest = os.path.join(self.dir, "readest")
        self.out = os.path.join(self.dir, "out")
        self.cache = os.path.join(self.dir, "cache.tsv")
        os.makedirs(self.readest)
        make_en_uk.save_cache(self.cache, {"кровавый": "кривавий", "шептать": "шепотіти"})
        self.write_pack(en_ru_pack())

    def tearDown(self):
        shutil.rmtree(self.dir)

    def write_pack(self, pack):
        raw = json.dumps(pack, ensure_ascii=False).encode("utf-8")
        with open(os.path.join(self.readest, "en-ru.json"), "wb") as f:
            f.write(raw)
        manifest = {
            "schemaVersion": 1,
            "packs": [
                {
                    "pair": "en-ru",
                    "source": "en",
                    "target": "ru",
                    "file": "en-ru.json",
                    "bytes": len(raw),
                    "sha256": hashlib.sha256(raw).hexdigest(),
                    "entries": len(pack["entries"]),
                }
            ],
        }
        with open(os.path.join(self.readest, "manifest.json"), "w") as f:
            json.dump(manifest, f)

    def run_update(self, *extra):
        argv = ["--readest-dir", self.readest, "--out", self.out, "--en-uk-cache", self.cache, "--offline", *extra]
        with _quiet(), contextlib.redirect_stdout(io.StringIO()):
            return update_packs.main(argv)

    def snapshot(self):
        """The archive's members, plus the raw archive and the plain ATTRIBUTION.md next to it."""
        out = pack_archive.read_archive(os.path.join(self.out, pack_archive.ARCHIVE_NAME))
        with open(os.path.join(self.out, pack_archive.ARCHIVE_NAME), "rb") as f:
            out["<archive>"] = f.read()
        with open(os.path.join(self.out, "ATTRIBUTION.md"), "rb") as f:
            out["<attribution>"] = f.read()
        return out

    def test_refresh_is_idempotent_and_incremental(self):
        self.assertEqual(self.run_update(), 0)
        first = self.snapshot()
        self.assertIn("word-lens-en-ru.lens", first)
        self.assertIn("word-lens-en-uk.lens", first)
        self.assertIn("word-lens-manifest.json", first)
        self.assertIn("ATTRIBUTION.md", first)
        self.assertEqual(first["ATTRIBUTION.md"], first["<attribution>"])
        self.assertEqual(sorted(os.listdir(self.out)), ["ATTRIBUTION.md", pack_archive.ARCHIVE_NAME])
        manifest = json.loads(first["word-lens-manifest.json"])
        ru = next(p for p in manifest["packs"] if p["pair"] == "en-ru")
        self.assertEqual(ru["sha256"], hashlib.sha256(first["word-lens-en-ru.lens"]).hexdigest())

        # A second run changes nothing, down to the archive's bytes.
        self.assertEqual(self.run_update(), 0)
        self.assertEqual(self.snapshot(), first)

        # The release staging step extracts and checks every asset.
        staged = os.path.join(self.dir, "staged")
        with _quiet(), contextlib.redirect_stdout(io.StringIO()) as printed:
            self.assertEqual(stage_release_assets.main(["--data", self.out, "--out", staged]), 0)
        self.assertEqual(printed.getvalue().strip(), "4")
        self.assertEqual(sorted(os.listdir(staged)), ["word-lens-ATTRIBUTION.md", "word-lens-en-ru.lens",
                                                      "word-lens-en-uk.lens", "word-lens-manifest.json"])

        changed = en_ru_pack()
        changed["entries"]["murmur"] = {"r": 8000, "g": "шепта́ть"}
        self.write_pack(changed)
        self.assertEqual(self.run_update("--dry-run"), 0)
        self.assertEqual(self.snapshot(), first)
        self.assertEqual(self.run_update(), 0)
        after = self.snapshot()
        self.assertNotEqual(after["word-lens-en-ru.lens"], first["word-lens-en-ru.lens"])
        lens = lens_format.LensFile(after["word-lens-en-uk.lens"])
        self.assertEqual(lens.lookup("murmur")[2], "шепотіти")

    def test_pairs_filter_and_verification_failure(self):
        self.assertEqual(self.run_update("--pairs", "en-ru"), 0)
        members = set(self.snapshot()) - {"<archive>", "<attribution>"}
        self.assertEqual(sorted(members), ["ATTRIBUTION.md", "word-lens-en-ru.lens", "word-lens-manifest.json"])
        with open(os.path.join(self.readest, "en-ru.json"), "ab") as f:
            f.write(b" ")
        self.assertNotEqual(self.run_update("--force"), 0)
        self.assertNotEqual(self.run_update("--pairs", "xx-yy"), 0)


if __name__ == "__main__":
    unittest.main()
