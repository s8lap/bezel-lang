// Codegen — string code-point reads must not over-read past a short string's
// buffer (issue #1848; §4.6 string storage / §8.3 hashing / §9.1 equality).
//
// loadCodePoint (LlvmBackend_String.cpp) decodes one code point from a string's
// fixed-width backing store. The earlier lowering computed ALL THREE width loads
// (i8 / i16 / i32) speculatively and selected the right one by the string's
// storage width. For a narrow string the i16/i32 loads read idx*2 / idx*4 bytes
// into a buffer that only holds idx*width bytes — an out-of-bounds READ past the
// tail of every short string (e.g. the width-4 load at idx=0 over "ab" reads
// bytes 0..3 of a 3-byte buffer). The discarded value made the computed hash /
// comparison correct, so it was benign in normal runs (plain `runAot`), but it
// is a genuine out-of-bounds read: the whole-suite generated-code ASan sweep
// reported ~83 of them (global/heap/stack buffer-overflow READ of size 4/2) in
// bz$valkey$hash / bz$reckey$hash and inline string ops. The fix branches on the
// actual width so ONLY the in-bounds load runs on each path.
//
// These tests run under generated-code AddressSanitizer (runAotChecked): the
// over-read flips r.ran to false (run() -> -999), so each program FAILS on HEAD
// and PASSES with the fix. Each also asserts the CORRECT result value — a fix
// that changed a hash / comparison would break the dictionary/set lookup and
// fail the same assertion. Keys are SHORT (1-2 chars) and word-boundary lengths
// so the over-read is guaranteed on the unfixed code.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + generated-code ASan
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §8.3 / §9.1 / #1848: a record with a `string` field used as a Dictionary key
// folds the field via the generated bz$reckey$hash (emitValueHashCode ->
// loadCodePoint). Short keys ("a","ab","cd") over-read past their 2-/3-byte
// literals on HEAD (global-buffer-overflow READ of size 4 in bz$reckey$hash).
// With the fix the run is ASan-clean AND the lookup still finds the equal key
// ("ab" -> 2), proving the hash/eq semantics are unchanged.
BEZEL_TEST(codegen_string_codepoint_oob_1848, record_string_key_short_no_overread) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Name(v: string); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<Name, int>(); "
        "  d[new Name(\"a\")] := 1; "
        "  d[new Name(\"ab\")] := 2; "
        "  d[new Name(\"cd\")] := 3; "
        "  var v: int? := null; d.TryGetValue(new Name(\"ab\"), out v); "
        "  return int(d.Count = 3 and v = 2); "
        "end Main; end C;"), 1);
}

// §4.10 / §8.3 / #1848: Set<string> routes short elements through the generic
// bz$valkey$hash (emitValueHashCode -> loadCodePoint). "a"/"ab" over-read on HEAD
// (global-buffer-overflow READ of size 4 in bz$valkey$hash). With the fix the run
// is ASan-clean AND dedup-by-content ("a" added twice -> Count 2) plus Contains
// still work, proving the hash is unchanged for equal strings.
BEZEL_TEST(codegen_string_codepoint_oob_1848, set_string_short_no_overread) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Set<string>(); "
        "  s.Add(\"a\"); s.Add(\"ab\"); s.Add(\"a\"); "
        "  return int(s.Count = 2 and s.Contains(\"ab\") and not s.Contains(\"zz\")); "
        "end Main; end C;"), 1);
}

// §4.6 / #1848: `for var ch in <string>` reads each code point via loadCodePoint.
// A short literal "ab" over-reads on HEAD (global-buffer-overflow READ of size 4
// in main). With the fix the loop is ASan-clean and each code point decodes
// correctly ('a'+'b' = 97+98 = 195).
BEZEL_TEST(codegen_string_codepoint_oob_1848, for_in_short_string_no_overread) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var total := 0; "
        "  for var ch in \"ab\" do total := total + int(ch); end; "
        "  return total; "                            // 97 + 98 = 195
        "end Main; end C;"), 195);
}

// §4.6 / #1848: the string indexer `s[i]` reads a code point via loadCodePoint.
// Indexing the last code point of a short string over-reads at the sub-word tail
// on HEAD (global-buffer-overflow READ of size 2 for the width-2 speculative
// load). With the fix it is ASan-clean and returns the right code point
// (s[1] of "ab" is 'b' = 98).
BEZEL_TEST(codegen_string_codepoint_oob_1848, string_index_last_no_overread) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var s := \"ab\"; "
        "  return int(s[1]); "                        // 'b' = 98
        "end Main; end C;"), 98);
}
