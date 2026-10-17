#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "modules/lingua/wordlens/LensFs.h"

// On-device Readest JSON pack -> .lens converter: the fallback when our release assets cannot be
// reached but Readest's CDN can (it only serves the JSON packs). Produces the same bytes as
// scripts/wordlens/lens_format.py's build_lens() for the same input.
//
// BOUNDED RAM. The JSON (1-2.5 MB) and the pack never sit in memory. Records are sorted by
// (fnv1a32(key), key), so the global order is a concatenation of hash-prefix partitions:
//   1. stream the JSON once: headword records go to the partition of hash(headword), each
//      headword's entry (lemma + cleaned gloss) is appended to the pool file in JSON order, and
//      every inflection pair goes to the partition of hash(LEMMA);
//   2. per partition: load its headwords, stream its inflections, resolve each lemma (rank + entry
//      offset) and append the resolved record to a second partition set, keyed by hash(FORM);
//   3. per partition: load its headwords plus its resolved inflections, sort, drop inflections
//      shadowed by a headword, append to the records file and fill the in-RAM bucket table;
//   4. assemble header + bucket table + records + pool + attribution, hashing as it copies.
// Peak RAM is one partition's headwords and resolved inflections (plus an 8-byte sort entry per
// record), the partition write buffers and the 2 KB JSON token buffer. 2^partitionLog2 partitions
// trade RAM for open files.
//
// INCREMENTAL. LensConverter does the work in bounded steps (a few KB of JSON, one partition, or a
// few KB of copying each), so a UI can run it from its loop with progress and cancel instead of
// blocking for the whole conversion.
//
// A JSON string longer than the parser's 2 KB token buffer is skipped by the parser; no Readest key
// or gloss comes near it (the longest are under 300 bytes), but a pack that did would no longer
// match the Python build byte for byte.
namespace lingua::wordlens {

enum class LensBuildPhase : uint8_t { Parse, Resolve, Sort, Assemble };

enum class LensBuildStatus : uint8_t {
  Ok,
  Cancelled,
  OpenFailed,
  ReadFailed,
  WriteFailed,
  BadJson,
  BadPack,
  OutOfMemory,
};

struct LensBuildOptions {
  // Directory for the temporary partition files (no trailing slash). Cleaned up when the build ends.
  const char* workDir = "";
  uint8_t partitionLog2 = 6;
  size_t writeBufferBytes = 256;
  void (*progress)(void* ctx, LensBuildPhase phase, uint32_t done, uint32_t total) = nullptr;
  void* progressCtx = nullptr;
  const bool* cancel = nullptr;
};

struct LensBuildStats {
  uint32_t headwords = 0;
  uint32_t inflectionPairs = 0;
  uint32_t inflectionsFolded = 0;
  uint32_t records = 0;
  uint32_t droppedEmptyGloss = 0;
  uint16_t bucketCount = 0;
  uint16_t maxBucketBytes = 0;
  uint32_t contentHash = 0;
  uint32_t outputBytes = 0;
};

class LensConverter {
 public:
  LensConverter();
  ~LensConverter();
  LensConverter(const LensConverter&) = delete;
  LensConverter& operator=(const LensConverter&) = delete;

  // Open the JSON and the temporary files. `fs` and the strings must outlive the conversion.
  LensBuildStatus begin(LensFs& fs, const char* jsonPath, const char* outPath, const LensBuildOptions& options);
  // One bounded unit of work. Ok while there is more to do (and once at the end, with done()
  // true); any other status ends the conversion with its temporary files removed and no output.
  LensBuildStatus step();
  bool done() const;
  // Stop and clean up; the output file is removed.
  void abort();

  LensBuildPhase phase() const;
  // Progress through the whole conversion, 0..100.
  uint8_t percent() const;
  const LensBuildStats& stats() const;

 private:
  struct State;
  std::unique_ptr<State> st_;
};

// Run a whole conversion at once (host tool and tests).
LensBuildStatus buildLensFromJson(LensFs& fs, const char* jsonPath, const char* outPath,
                                  const LensBuildOptions& options, LensBuildStats* stats = nullptr);

// Partition count for a JSON pack of `jsonBytes`, aiming at ~16 KB of records per partition.
uint8_t recommendedPartitionLog2(uint32_t jsonBytes);

const char* lensBuildStatusName(LensBuildStatus status);

}  // namespace lingua::wordlens
