// Codegen / leak — the static `string` factory methods that mint a fresh heap
// `string` (§9.6) leaked their result when bound to a local, fed into ??, or
// read via `.Length`. Issue #2228 — the STATIC-call sibling of the
// instance-string-transforming gap #2199; the same family as the ToString-family
// gaps #1982 / #1988 / #1989 / #2065.
//
// Spec: stdlib §9.6 (`string` static construction methods:
//   string.From(c: char): string
//   string.From(buffer: Array<char>, start: int, count: int): string
//   string.Repeat(c: char, count: int): string
// each return a freshly allocated `string`), language spec §4.6 (runtime-
// managed `string` refcount), §28.3 (producers), §28.9 (runtime-managed
// types). Spec revision v0.226.
//
// THE BUG. `isPlusOneStringProducer` (LlvmBackend_Setup.cpp) enumerated ONLY
// the static `string.Join` form (the #1184-era work) — the other three static
// `string` factories that lower (in `lowerStaticCall`,
// LlvmBackend_Calls2.cpp ~line 466 / 485 / 622) to a freshly-malloc'd
// `bz.string` at `rc := 1` (`emitPrimitiveToString` → `bz_char_to_string`,
// `bz_str_from_char_array`, and the inline `malloc + initStringRc` for Repeat)
// were absent. So the result was treated as a BORROW: a bound local retained
// (rc 1→2) and released (rc 2→1) at scope end — the producer's original +1
// was never decremented, leaking the 32-byte bz_string header + its data
// buffer. The #2228 repro:
//   string.From('A')                  → 34 bytes leaked (32 header + 2 data)
//   string.Repeat('A', 100)           → 133 bytes leaked
//   string.From(buf, 0, 3)            → 36 bytes leaked (32 header + 4 data)
// The corresponding #2199 instance methods (ToUpper/ToLower/Trim/Substring/...)
// are CLEAN — they're in the recognition list. The static-call recognition
// for these three factories was the residual from the same gap.
//
// THE FIX. Extend `isPlusOneStringProducer` to recognize the three missing
// static factories by the SAME shape `lowerStaticCall` keys on — a `CallExpr`
// whose callee is a `MemberAccessExpr` on the `string`/`String` type keyword
// with member `From` (1 char arg or 3 buffer/start/count args) or `Repeat` (2
// char/count args) — so the recognition and the lowering stay in lockstep.
// A bound local TRANSFERS the single reference (no extra retain) and the
// scope-end release — when the escape walk proves the local non-escaping —
// drops rc 1→0 exactly once. No double-free: an escaping local is excluded
// from the release by the escape walk (its +1 then leaks — the safe
// direction).
//
// OBSERVABILITY. runAotChecked links + runs the generated code under ASan/LSan;
// a leaked string flips r.ran to false. Each case renders in a LOOP and
// counts byte-correct results — so the exit code pins the VALUE and r.ran
// pins leak-freedom. Each case FAILS without the fix (LSan reports the
// leaked strings → r.ran false) and PASSES with it.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

}  // namespace

// §9.6 string.From(char) → rc=1 producer. LSan pin: 34 bytes leaked pre-fix
// (32-byte bz.string header + 2-byte "A" data buffer), from bz_char_to_string
// at src/runtime/bezel_runtime_strings.cpp:246.
BEZEL_TEST(stdlib_string_static_factories_leak_2228,
           from_char_lsan_clean_stdlib_9_6) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var count := 0; "
        "  var i := 0; "
        "  while i < 40 do "
        "    var s := string.From('A'); "
        "    if s.Length = 1 then count := count + 1; end; "
        "    i := i + 1; "
        "  end; "
        "  return count; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// §9.6 string.Repeat(char, count) → rc=1 producer. LSan pin: 133 bytes
// leaked pre-fix (32-byte header + 101-byte buffer including NUL terminator),
// from the inline malloc + initStringRc path in
// LlvmBackend_Calls2.cpp's Repeat block.
BEZEL_TEST(stdlib_string_static_factories_leak_2228,
           repeat_char_int_lsan_clean_stdlib_9_6) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var count := 0; "
        "  var i := 0; "
        "  while i < 40 do "
        "    var s := string.Repeat('A', 100); "
        "    if s.Length = 100 then count := count + 1; end; "
        "    i := i + 1; "
        "  end; "
        "  return count; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// §9.6 string.From(Array<char>, start, count) → rc=1 producer. LSan pin:
// 36 bytes leaked pre-fix (32-byte header + 4-byte data buffer "abc\0"),
// from bz_str_from_char_array at
// src/runtime/bezel_runtime_strings.cpp:532.
BEZEL_TEST(stdlib_string_static_factories_leak_2228,
           from_buffer_start_count_lsan_clean_stdlib_9_6) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  using var buf := new Array<char>(); "
        "  buf.Add('a'); buf.Add('b'); buf.Add('c'); "
        "  var count := 0; "
        "  var i := 0; "
        "  while i < 40 do "
        "    var s := string.From(buf, 0, 3); "
        "    if s.Length = 3 then count := count + 1; end; "
        "    i := i + 1; "
        "  end; "
        "  return count; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// §9.6 string.Join (control — already recognized before #2228) — must stay
// clean after the fix. Pins that the recognition extension does not regress
// the pre-existing #1184 path.
BEZEL_TEST(stdlib_string_static_factories_leak_2228,
           join_still_clean_stdlib_9_6) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  using var items := new Array<string>(); "
        "  items.Add(\"a\"); items.Add(\"b\"); "
        "  var count := 0; "
        "  var i := 0; "
        "  while i < 40 do "
        "    var s := string.Join(\",\", items); "
        "    if s.Length = 3 then count := count + 1; end; "
        "    i := i + 1; "
        "  end; "
        "  return count; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// §9.6 string.From(char) consumed by `??` — the consumer that decides
// retain-vs-transfer for the result (the `??` arm normalization in #1948 /
// #1952) must classify `string.From('A')` as a +1 producer so the result
// is transferred (not borrow-retained, which would leak). Mirrors the
// #2199 `p ?? s.ToUpper()` shape.
BEZEL_TEST(stdlib_string_static_factories_leak_2228,
           from_char_in_coalesce_lsan_clean_stdlib_9_6) {
    const std::string src =
        "module M; class C "
        "  public static method Main(): int "
        "    var count := 0; "
        "    var i := 0; "
        "    while i < 40 do "
        "      var p: string? := null; "
        "      var s: string := p ?? string.From('B'); "
        "      if s.Length = 1 then count := count + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}
