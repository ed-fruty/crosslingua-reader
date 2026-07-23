// Host-side unit test for the canonical text fold (src/translator/TextNormalize).
// Built and run via the CMake native-test harness (test/CMakeLists.txt ->
// add_subdirectory(text_normalize)). No device/framework deps; its own main()
// returns non-zero on failure so CTest reports pass/fail.

#include <cstdio>
#include <string>

#include "TextNormalize.h"

using textnorm::closingQuoteLenAt;
using textnorm::ELLIPSIS_SENTINEL;
using textnorm::foldForMatch;
using textnorm::foldForMatchInPlace;
using textnorm::terminatorLenAt;
using textnorm::whitespaceLenAt;

static int g_failures = 0;
static int g_checks = 0;

// Render a byte string with non-printable bytes shown as <NN> so failures read.
static std::string show(const std::string& s) {
  std::string out;
  char buf[8];
  for (unsigned char c : s) {
    if (c >= 0x20 && c < 0x7F) {
      out += static_cast<char>(c);
    } else {
      std::snprintf(buf, sizeof(buf), "<%02X>", c);
      out += buf;
    }
  }
  return out;
}

static void expectEq(const char* name, const std::string& got, const std::string& want) {
  g_checks++;
  if (got != want) {
    g_failures++;
    std::printf("FAIL %-32s got=\"%s\" want=\"%s\"\n", name, show(got).c_str(), show(want).c_str());
  }
}

static void expectInt(const char* name, int got, int want) {
  g_checks++;
  if (got != want) {
    g_failures++;
    std::printf("FAIL %-32s got=%d want=%d\n", name, got, want);
  }
}

int main() {
  const std::string ELL(1, ELLIPSIS_SENTINEL);

  // ── Ukrainian punctuation set (the acceptance target from the plan) ────────
  // « » (U+00AB / U+00BB) -> "
  expectEq("uk_angle_quotes",
           foldForMatch("\xC2\xAB"
                        "Привіт"
                        "\xC2\xBB"),
           "\"Привіт\"");
  // Ukrainian apostrophe both as U+2019 (’) and U+02BC (ʼ) -> '
  expectEq("uk_apostrophe_2019",
           foldForMatch("\xD0\xBF"
                        "\xE2\x80\x99"
                        "\xD1\x8F\xD1\x82\xD1\x8C"),
           "\xD0\xBF"
           "'"
           "\xD1\x8F\xD1\x82\xD1\x8C");
  expectEq("uk_apostrophe_02bc",
           foldForMatch("\xD0\xBF"
                        "\xCA\xBC"
                        "\xD1\x8F\xD1\x82\xD1\x8C"),
           "\xD0\xBF"
           "'"
           "\xD1\x8F\xD1\x82\xD1\x8C");
  // em dash — (U+2014) -> -
  expectEq("uk_em_dash",
           foldForMatch("a\xE2\x80\x94"
                        "b"),
           "a-b");
  // horizontal ellipsis … (U+2026) -> sentinel
  expectEq("uk_ellipsis", foldForMatch("wait\xE2\x80\xA6"), "wait" + ELL);

  // ── Double-quote variants all fold to " ────────────────────────────────────
  expectEq("dq_left_2201C", foldForMatch("\xE2\x80\x9C"), "\"");  // “
  expectEq("dq_right_201D", foldForMatch("\xE2\x80\x9D"), "\"");  // ”
  expectEq("dq_low_201E", foldForMatch("\xE2\x80\x9E"), "\"");    // „
  expectEq("dq_high_201F", foldForMatch("\xE2\x80\x9F"), "\"");   // ‟

  // ── Single-quote variants all fold to ' ─────────────────────────────────────
  expectEq("sq_left_2018", foldForMatch("\xE2\x80\x98"), "'");   // ‘
  expectEq("sq_right_2019", foldForMatch("\xE2\x80\x99"), "'");  // ’
  expectEq("sq_low_201A", foldForMatch("\xE2\x80\x9A"), "'");    // ‚

  // ── Dash variants all fold to - ─────────────────────────────────────────────
  expectEq("dash_hyphen_2010", foldForMatch("\xE2\x80\x90"), "-");  // ‐
  expectEq("dash_en_2013", foldForMatch("\xE2\x80\x93"), "-");      // –
  expectEq("dash_em_2014", foldForMatch("\xE2\x80\x94"), "-");      // —
  expectEq("dash_minus_2212", foldForMatch("\xE2\x88\x92"), "-");   // −

  // ── Ellipsis forms converge: "..." == "…" == "...." ────────────────────────
  expectEq("ellipsis_ascii3", foldForMatch("a...b"), "a" + ELL + "b");
  expectEq("ellipsis_ascii2", foldForMatch("a..b"), "a" + ELL + "b");
  expectEq("ellipsis_ascii4", foldForMatch("a....b"), "a" + ELL + "b");
  expectEq("ellipsis_converge", foldForMatch("a...b"),
           foldForMatch("a\xE2\x80\xA6"
                        "b"));
  expectEq("lone_dot_kept", foldForMatch("a.b"), "a.b");

  // ── NBSP / soft hyphen / whitespace behavior (subsumes normalizeForMatch) ──
  expectEq("nbsp_to_space",
           foldForMatch("a\xC2\xA0"
                        "b"),
           "a b");
  expectEq("soft_hyphen_drop",
           foldForMatch("ab\xC2\xAD"
                        "cd"),
           "abcd");
  expectEq("ws_collapse_trim", foldForMatch("   a \t\n  b   "), "a b");
  expectEq("nbsp_collapse_with_ws", foldForMatch("a \xC2\xA0 b"), "a b");

  // ── Passthrough: unrecognized multibyte survives verbatim ───────────────────
  expectEq("cyrillic_passthrough", foldForMatch("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD1\x96\xD1\x82"),
           "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD1\x96\xD1\x82");
  // Other 0xC2 (¶ U+00B6) and 0xE2 (€ U+20AC) survive.
  expectEq("c2_other_passthrough", foldForMatch("\xC2\xB6"), "\xC2\xB6");
  expectEq("e2_other_passthrough", foldForMatch("\xE2\x82\xAC"), "\xE2\x82\xAC");

  // ── Truncated trailing lead bytes are emitted literally (no over-read) ──────
  expectEq("trunc_e2", foldForMatch("x\xE2"), "x\xE2");
  expectEq("trunc_c2", foldForMatch("x\xC2"), "x\xC2");

  // ── limit caps output length ────────────────────────────────────────────────
  expectEq("limit_cap", foldForMatch("abcdefgh", 4), "abcd");
  expectEq("limit_zero", foldForMatch("abc", 0), "");

  // ── Idempotence: folding folded text is a no-op ─────────────────────────────
  {
    const std::string once = foldForMatch(
        "\xC2\xAB"
        "He said \xE2\x80\x94 wait\xE2\x80\xA6\xC2\xBB");
    expectEq("idempotent", foldForMatch(once), once);
  }

  // ── In-place variant matches the copy variant ───────────────────────────────
  {
    std::string in =
        "  \xC2\xAB"
        "Test\xE2\x80\xA6\xC2\xBB  ";
    const std::string copy = foldForMatch(in);
    foldForMatchInPlace(in);
    expectEq("inplace_matches_copy", in, copy);
  }

  // ── terminatorLenAt ─────────────────────────────────────────────────────────
  expectInt("term_period", terminatorLenAt(".", 0), 1);
  expectInt("term_bang", terminatorLenAt("!", 0), 1);
  expectInt("term_qmark", terminatorLenAt("?", 0), 1);
  expectInt("term_sentinel", terminatorLenAt(ELL, 0), 1);
  expectInt("term_letter", terminatorLenAt("a", 0), 0);
  expectInt("term_cjk_period", terminatorLenAt("\xE3\x80\x82", 0), 3);  // 。
  expectInt("term_cjk_bang", terminatorLenAt("\xEF\xBC\x81", 0), 3);    // ！
  expectInt("term_cjk_qmark", terminatorLenAt("\xEF\xBC\x9F", 0), 3);   // ？
  expectInt("term_oob", terminatorLenAt("abc", 5), 0);

  // ── closingQuoteLenAt (post-fold: ASCII only) ───────────────────────────────
  expectInt("cq_dquote", closingQuoteLenAt("\"", 0), 1);
  expectInt("cq_squote", closingQuoteLenAt("'", 0), 1);
  expectInt("cq_paren", closingQuoteLenAt(")", 0), 1);
  expectInt("cq_bracket", closingQuoteLenAt("]", 0), 1);
  expectInt("cq_none", closingQuoteLenAt("a", 0), 0);

  // ── whitespaceLenAt (SSOT for inter-token separators) ───────────────────────
  expectInt("ws_ascii_space", whitespaceLenAt(" ", 0), 1);
  expectInt("ws_tab", whitespaceLenAt("\t", 0), 1);
  expectInt("ws_lf", whitespaceLenAt("\n", 0), 1);
  expectInt("ws_cr", whitespaceLenAt("\r", 0), 1);
  expectInt("ws_nbsp", whitespaceLenAt("\xC2\xA0", 0), 2);             // U+00A0
  expectInt("ws_en_space", whitespaceLenAt("\xE2\x80\x82", 0), 3);     // U+2002
  expectInt("ws_em_space", whitespaceLenAt("\xE2\x80\x83", 0), 3);     // U+2003
  expectInt("ws_thin_space", whitespaceLenAt("\xE2\x80\x89", 0), 3);   // U+2009
  expectInt("ws_2000", whitespaceLenAt("\xE2\x80\x80", 0), 3);         // U+2000 (low edge)
  expectInt("ws_200a", whitespaceLenAt("\xE2\x80\x8A", 0), 3);         // U+200A (high edge)
  expectInt("ws_narrow_nbsp", whitespaceLenAt("\xE2\x80\xAF", 0), 3);  // U+202F
  expectInt("ws_math_space", whitespaceLenAt("\xE2\x81\x9F", 0), 3);   // U+205F
  expectInt("ws_letter", whitespaceLenAt("a", 0), 0);
  expectInt("ws_dot", whitespaceLenAt(".", 0), 0);
  expectInt("ws_not_space_200b", whitespaceLenAt("\xE2\x80\x8B", 0), 0);  // U+200B zero-width (not ws)
  expectInt("ws_oob", whitespaceLenAt("abc", 5), 0);
  expectInt("ws_trunc_c2", whitespaceLenAt("\xC2", 0), 0);      // lone C2 (no A0)
  expectInt("ws_trunc_e2", whitespaceLenAt("\xE2\x80", 0), 0);  // truncated 3-byte space

  // ── Fold collapses the same Unicode spaces to a single ASCII space ──────────
  expectEq("fold_nbsp",
           foldForMatch("a\xC2\xA0"
                        "b"),
           "a b");
  expectEq("fold_narrow_nbsp",
           foldForMatch("a\xE2\x80\xAF"
                        "b"),
           "a b");
  expectEq("fold_en_space",
           foldForMatch("a\xE2\x80\x82"
                        "b"),
           "a b");
  expectEq("fold_math_space",
           foldForMatch("a\xE2\x81\x9F"
                        "b"),
           "a b");
  // Mixed run of ASCII + Unicode spaces collapses to one space and trims ends.
  expectEq("fold_mixed_space_run",
           foldForMatch("\xE2\x80\x83  a\xC2\xA0\xE2\x80\xAF"
                        "b \xE2\x80\x82"),
           "a b");

  if (g_failures == 0) {
    std::printf("OK: all %d checks passed\n", g_checks);
    return 0;
  }
  std::printf("FAILED: %d/%d checks failed\n", g_failures, g_checks);
  return 1;
}
