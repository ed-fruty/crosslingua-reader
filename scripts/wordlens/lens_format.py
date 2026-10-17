"""Word Lens `.lens` pack format: gloss cleaning, writer and reader.

This module is the reference implementation of the on-device format. The firmware's
streaming converter (src/modules/lingua/wordlens/LensBuilder.cpp) implements the SAME
algorithm and must produce byte-identical files from the same Readest JSON pack; the
host test test/word_lens_format checks that on a fixture. Change both together and bump
FORMAT_VERSION when the bytes change.

Layout (all integers little-endian, see docs/file-formats.md "Word Lens pack (.lens)"):

    header (64 bytes)
    bucket table: (bucketCount + 1) x u32, offsets relative to the records section
    records: per record  keyLen:u8  key[keyLen]  rank:u16  glossOff:u24
             globally sorted by (fnv1a32(key), key bytes); bucket = fnv1a32(key) >> (32 - log2(bucketCount))
    gloss pool: per headword  headLen:u8  head[headLen]  glossLen:u8  gloss[glossLen]
                (in JSON entry order of the kept headwords; an inflection record points at its
                lemma's entry, so the entry also names the lemma)
    attribution: UTF-8 text
"""

from __future__ import annotations

import json
import struct
from dataclasses import dataclass

MAGIC = b"LENS"
FORMAT_VERSION = 1
HEADER_SIZE = 64
MAX_BUCKETS = 1024
RECORDS_PER_BUCKET = 48
MAX_GLOSS_CODEPOINTS = 24
MAX_GLOSS_BYTES = 240
MAX_SENSES = 3
SENSE_JOINER = "; "
MAX_KEY_BYTES = 255
MAX_RANK = 0xFFFF
MAX_POOL_BYTES = 1 << 24

FLAG_MONOLINGUAL = 0x01

RANK_SCALE_FREQUENCY = 0
RANK_SCALE_HSK = 1

ELLIPSIS = "…"

DEFAULT_ATTRIBUTION = (
    "Word Lens data derived from Readest's Word Lens packs (https://github.com/readest/readest). "
    "See ATTRIBUTION.md for sources and licences."
)

# Characters removed from a gloss wherever they occur: Cyrillic stress / accent marks and
# other combining diacritics the small annotation faces do not position well, and Arabic
# vowel signs (the reader's text is normally unvocalized, and at annotation size the marks
# collide with the row above).
STRIPPED_MARK_RANGES = (
    (0x0300, 0x036F),
    (0x0483, 0x0487),
    (0x064B, 0x0652),
    (0x0670, 0x0670),
)

WHITESPACE = {" ", "\t", "\n", "\r", " "}
SENSE_SEPARATORS_CROSS = {"；", ";", "/"}
SENSE_SEPARATORS_MONO = {"；", ";"}
SYNONYM_SEPARATORS = {",", "，", "、"}
EDGE_TRIM = {" ", ",", ";", ":", "–", "—"}


def fnv1a32(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def _is_stripped_mark(ch: str) -> bool:
    cp = ord(ch)
    return any(lo <= cp <= hi for lo, hi in STRIPPED_MARK_RANGES)


def _remove_spans(text: str, open_ch: str, close_ch: str) -> str:
    """Delete matched open..close spans; an unmatched opener is deleted on its own."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == open_ch:
            j = text.find(close_ch, i + 1)
            if j >= 0:
                i = j + 1
                continue
            i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def _collapse_whitespace(text: str) -> str:
    out = []
    pending_space = False
    for ch in text:
        if ch in WHITESPACE:
            pending_space = True
            continue
        if pending_space and out:
            out.append(" ")
        pending_space = False
        out.append(ch)
    return "".join(out)


def _strip_leading_pos(text: str) -> str:
    """Remove leading dictionary part-of-speech tags such as "v." / "adj. " / "interj."."""
    while True:
        i = 0
        while i < len(text) and text[i] in WHITESPACE:
            i += 1
        j = i
        while j < len(text) and j - i < 7 and ("a" <= text[j] <= "z" or "A" <= text[j] <= "Z"):
            j += 1
        letters = j - i
        if 1 <= letters <= 6 and j < len(text) and text[j] == ".":
            k = j + 1
            while k < len(text) and text[k] in WHITESPACE:
                k += 1
            text = text[k:]
            continue
        return text[i:] if i else text


def _cut_classifier(text: str) -> str:
    """Drop a CC-CEDICT "CL:" classifier clause and everything after it."""
    start = 0
    while True:
        k = text.find("CL:", start)
        if k < 0:
            return text
        if k == 0 or not text[k - 1].isascii() or not text[k - 1].isalnum():
            return text[:k]
        start = k + 1


def _trim_edges(text: str) -> str:
    a, b = 0, len(text)
    while a < b and text[a] in EDGE_TRIM:
        a += 1
    while b > a and text[b - 1] in EDGE_TRIM:
        b -= 1
    return text[a:b]


def _split_any(text: str, seps) -> list[str]:
    parts = []
    cur = []
    for ch in text:
        if ch in seps:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return parts


def clean_sense(sense: str, monolingual: bool) -> str:
    """One sense, markup and marks stripped. Empty when nothing readable is left."""
    # Wiktionary link markup "[[word]]" keeps its text; a single-bracket "[...]" span is a
    # pinyin / domain annotation and goes with its content.
    s = sense.replace("[[", "").replace("]]", "")
    s = _remove_spans(s, "[", "]")
    s = "".join(ch for ch in s if ch not in "]{}|")
    s = _remove_spans(s, "(", ")")
    s = s.replace(")", "")
    s = "".join(ch for ch in s if not _is_stripped_mark(ch))
    if not monolingual:
        s = _strip_leading_pos(s)
        s = _cut_classifier(s)
    s = _collapse_whitespace(s)
    s = s.strip(" ")
    if not monolingual:
        s = _split_any(s, SYNONYM_SEPARATORS)[0].strip(" ")
    return _trim_edges(s)


def _truncate_utf8(text: str, max_bytes: int) -> str:
    if len(text.encode("utf-8")) <= max_bytes:
        return text
    budget = max_bytes - len(ELLIPSIS.encode("utf-8"))
    out = []
    used = 0
    for ch in text:
        n = len(ch.encode("utf-8"))
        if used + n > budget:
            break
        out.append(ch)
        used += n
    return "".join(out).rstrip(" ") + ELLIPSIS


def clean_gloss(raw: str, monolingual: bool) -> str:
    """The stored gloss: up to MAX_SENSES cleaned senses joined by SENSE_JOINER.

    The first sense is what the annotation strip shows (capped to MAX_GLOSS_CODEPOINTS at
    layout time); the whole string is what the reader's gloss popup shows.
    """
    seps = SENSE_SEPARATORS_MONO if monolingual else SENSE_SEPARATORS_CROSS
    senses: list[str] = []
    total = 0
    for sense in _split_any(raw, seps):
        s = clean_sense(sense, monolingual)
        if not s or s in senses:
            continue
        if not senses:
            s = _truncate_utf8(s, MAX_GLOSS_BYTES)
            senses.append(s)
            total = len(s.encode("utf-8"))
        else:
            add = len(SENSE_JOINER.encode("utf-8")) + len(s.encode("utf-8"))
            if total + add > MAX_GLOSS_BYTES:
                break
            senses.append(s)
            total += add
        if len(senses) >= MAX_SENSES:
            break
    return SENSE_JOINER.join(senses)


def display_gloss(stored: str) -> str:
    """What the annotation strip shows: the first sense, capped to MAX_GLOSS_CODEPOINTS."""
    first = stored.split(SENSE_JOINER, 1)[0]
    if len(first) > MAX_GLOSS_CODEPOINTS:
        first = first[: MAX_GLOSS_CODEPOINTS - 1].rstrip(" ") + ELLIPSIS
    return first


def bucket_count_for(record_estimate: int) -> int:
    want = (record_estimate + RECORDS_PER_BUCKET - 1) // RECORDS_PER_BUCKET
    b = 1
    while b < want and b < MAX_BUCKETS:
        b <<= 1
    return b


def _log2(n: int) -> int:
    return n.bit_length() - 1


def bucket_of(key_hash: int, bucket_log2: int) -> int:
    return 0 if bucket_log2 == 0 else key_hash >> (32 - bucket_log2)


def attribution_for(meta: dict) -> str:
    text = str(meta.get("attribution") or "").strip()
    lic = str(meta.get("license") or "").strip()
    if lic:
        text = (text + " " if text else "") + "License: " + lic + "."
    return text or DEFAULT_ATTRIBUTION


def _pad_lang(code: str) -> bytes:
    raw = code.encode("ascii")
    if len(raw) > 7:
        raise ValueError(f"language code too long: {code!r}")
    return raw + b"\0" * (8 - len(raw))


@dataclass
class BuildStats:
    headwords: int
    inflections_folded: int
    records: int
    dropped_empty_gloss: int
    bucket_count: int
    max_bucket_bytes: int


def build_lens(pack: dict, entries_pairs=None, inflection_pairs=None, attribution: str | None = None):
    """Build a .lens image from a Readest pack dict. Returns (bytes, BuildStats).

    entries_pairs / inflection_pairs default to the dict order of pack["entries"] /
    pack["inflections"], which is the JSON file order (json.load keeps it).
    """
    meta = pack.get("meta", {})
    source = meta["source"]
    target = meta["target"]
    monolingual = source == target
    rank_scale = RANK_SCALE_HSK if source == "zh" else RANK_SCALE_FREQUENCY
    if entries_pairs is None:
        entries_pairs = list(pack["entries"].items())
    if inflection_pairs is None:
        inflection_pairs = list(pack.get("inflections", {}).items())

    pool = bytearray()
    headwords: dict[str, tuple[int, int]] = {}
    dropped = 0
    for key, value in entries_pairs:
        kb = key.encode("utf-8")
        if not kb or len(kb) > MAX_KEY_BYTES:
            continue
        gloss = clean_gloss(str(value.get("g", "")), monolingual)
        if not gloss:
            dropped += 1
            continue
        rank = int(value.get("r", MAX_RANK))
        rank = max(0, min(MAX_RANK, rank))
        gb = gloss.encode("utf-8")
        off = len(pool)
        pool.append(len(kb))
        pool += kb
        pool.append(len(gb))
        pool += gb
        headwords[key] = (rank, off)
    if len(pool) >= MAX_POOL_BYTES:
        raise ValueError("gloss pool exceeds 16 MiB")

    records: dict[str, tuple[int, int]] = dict(headwords)
    folded = 0
    for form, lemma in inflection_pairs:
        fb = form.encode("utf-8")
        if not fb or len(fb) > MAX_KEY_BYTES:
            continue
        if form in headwords or form in records:
            continue
        target_entry = headwords.get(lemma)
        if target_entry is None:
            continue
        records[form] = target_entry
        folded += 1

    bucket_count = bucket_count_for(len(headwords) + len(inflection_pairs))
    blog = _log2(bucket_count)

    keyed = []
    for key, (rank, off) in records.items():
        kb = key.encode("utf-8")
        keyed.append((fnv1a32(kb), kb, rank, off))
    keyed.sort(key=lambda r: (r[0], r[1]))

    rec_bytes = bytearray()
    table = [0] * (bucket_count + 1)
    current = 0
    for h, kb, rank, off in keyed:
        b = bucket_of(h, blog)
        while current < b:
            current += 1
            table[current] = len(rec_bytes)
        rec_bytes.append(len(kb))
        rec_bytes += kb
        rec_bytes += struct.pack("<H", rank)
        rec_bytes += bytes((off & 0xFF, (off >> 8) & 0xFF, (off >> 16) & 0xFF))
    while current < bucket_count:
        current += 1
        table[current] = len(rec_bytes)
    max_bucket = max((table[i + 1] - table[i] for i in range(bucket_count)), default=0)
    if max_bucket > 0xFFFF:
        raise ValueError("bucket larger than 64 KiB")

    attr = (attribution if attribution is not None else attribution_for(meta)).encode("utf-8")
    if len(attr) > 0xFFFF:
        raise ValueError("attribution too long")

    table_bytes = b"".join(struct.pack("<I", v) for v in table)
    records_off = HEADER_SIZE + len(table_bytes)
    pool_off = records_off + len(rec_bytes)
    attr_off = pool_off + len(pool)
    body = table_bytes + bytes(rec_bytes) + bytes(pool) + attr
    content_hash = fnv1a32(body)

    flags = FLAG_MONOLINGUAL if monolingual else 0
    header = bytearray(HEADER_SIZE)
    header[0:4] = MAGIC
    header[4] = FORMAT_VERSION
    header[5] = flags
    struct.pack_into("<H", header, 6, HEADER_SIZE)
    header[8:16] = _pad_lang(source)
    header[16:24] = _pad_lang(target)
    struct.pack_into("<I", header, 24, len(keyed))
    struct.pack_into("<I", header, 28, len(headwords))
    struct.pack_into("<H", header, 32, bucket_count)
    header[34] = blog
    header[35] = rank_scale
    struct.pack_into("<I", header, 36, HEADER_SIZE)
    struct.pack_into("<I", header, 40, records_off)
    struct.pack_into("<I", header, 44, pool_off)
    struct.pack_into("<I", header, 48, len(pool))
    struct.pack_into("<I", header, 52, attr_off)
    struct.pack_into("<H", header, 56, len(attr))
    struct.pack_into("<H", header, 58, max_bucket)
    struct.pack_into("<I", header, 60, content_hash)

    stats = BuildStats(
        headwords=len(headwords),
        inflections_folded=folded,
        records=len(keyed),
        dropped_empty_gloss=dropped,
        bucket_count=bucket_count,
        max_bucket_bytes=max_bucket,
    )
    return bytes(header) + body, stats


def load_pack_json(path: str) -> dict:
    """Load a Readest pack, refusing duplicate keys (the device converter could not mirror json's
    last-wins rule for them)."""

    def hook(pairs):
        seen = set()
        for k, _ in pairs:
            if k in seen:
                raise ValueError(f"duplicate key {k!r} in {path}")
            seen.add(k)
        return dict(pairs)

    with open(path, "r", encoding="utf-8") as f:
        return json.load(f, object_pairs_hook=hook)


class LensFile:
    """Reader used by the build scripts to verify packs and print samples."""

    def __init__(self, data: bytes):
        if data[0:4] != MAGIC:
            raise ValueError("not a .lens file")
        self.data = data
        self.version = data[4]
        self.flags = data[5]
        self.source = data[8:16].rstrip(b"\0").decode("ascii")
        self.target = data[16:24].rstrip(b"\0").decode("ascii")
        (self.record_count,) = struct.unpack_from("<I", data, 24)
        (self.headword_count,) = struct.unpack_from("<I", data, 28)
        (self.bucket_count,) = struct.unpack_from("<H", data, 32)
        self.bucket_log2 = data[34]
        self.rank_scale = data[35]
        (self.table_off,) = struct.unpack_from("<I", data, 36)
        (self.records_off,) = struct.unpack_from("<I", data, 40)
        (self.pool_off,) = struct.unpack_from("<I", data, 44)
        (self.pool_size,) = struct.unpack_from("<I", data, 48)
        (self.attr_off,) = struct.unpack_from("<I", data, 52)
        (self.attr_len,) = struct.unpack_from("<H", data, 56)
        (self.max_bucket,) = struct.unpack_from("<H", data, 58)
        (self.content_hash,) = struct.unpack_from("<I", data, 60)
        if fnv1a32(data[HEADER_SIZE:]) != self.content_hash:
            raise ValueError("content hash mismatch")

    @property
    def attribution(self) -> str:
        return self.data[self.attr_off : self.attr_off + self.attr_len].decode("utf-8")

    def entry_at(self, off: int) -> tuple[str, str]:
        p = self.pool_off + off
        n = self.data[p]
        head = self.data[p + 1 : p + 1 + n].decode("utf-8")
        p += 1 + n
        m = self.data[p]
        return head, self.data[p + 1 : p + 1 + m].decode("utf-8")

    def lookup(self, word: str):
        kb = word.encode("utf-8")
        b = bucket_of(fnv1a32(kb), self.bucket_log2)
        start, end = struct.unpack_from("<II", self.data, self.table_off + 4 * b)
        p = self.records_off + start
        stop = self.records_off + end
        while p < stop:
            n = self.data[p]
            key = self.data[p + 1 : p + 1 + n]
            rank = struct.unpack_from("<H", self.data, p + 1 + n)[0]
            o = p + 3 + n
            off = self.data[o] | (self.data[o + 1] << 8) | (self.data[o + 2] << 16)
            if key == kb:
                lemma, gloss = self.entry_at(off)
                return rank, lemma, gloss
            p += n + 6
        return None

    def iter_records(self):
        """Yield (key, rank, lemma, gloss) for every record, in file order."""
        p = self.records_off
        stop = self.pool_off
        while p < stop:
            n = self.data[p]
            key = self.data[p + 1 : p + 1 + n].decode("utf-8")
            rank = struct.unpack_from("<H", self.data, p + 1 + n)[0]
            o = p + 3 + n
            off = self.data[o] | (self.data[o + 1] << 8) | (self.data[o + 2] << 16)
            lemma, gloss = self.entry_at(off)
            yield key, rank, lemma, gloss
            p += n + 6
