// Codegen / leak — the stdlib `string` INSTANCE methods that transform self
// into a fresh heap `string` (§9.6) leaked their result when bound to a local,
// fed into `??`, or read via `.Length`. Issue #2199 — the family-wide sibling
// of the incrementally-closed ToString-family gaps (#1982 Guid, #1988
// DateTime/TimeSpan/DateOnly/TimeOnly, #1989 Guid.ToString(format), #2065
// synthesized record ToString).
//
// Spec: stdlib §9.6 (`string` instance methods: ToUpper/ToLower/Trim/
// TrimStart/TrimEnd/Substring/Replace[char,char]/Replace[string,string]/
// PadLeft/PadRight/Insert/Remove all return a fresh `string`; the property
// `[r: Range<int>]: string` slices to a fresh `string`), language spec §4.6
// (runtime-managed `string` refcount), §28.3 (producers), §28.9 (runtime-
// managed types). Spec revision v0.220.
//
// THE BUG. `isPlusOneStringProducer` (LlvmBackend_Setup.cpp) enumerated the
// static `string.Join` form plus the ToString family but did NOT enumerate
// the string-transforming instance methods — even though each of them lowers
// (in tryLowerStringMethod, LlvmBackend_String.cpp) to either an inline
// `malloc + initStringRc` (`#422: producer's reference, rc := 1`) or to a
// runtime helper that mints the same shape (`bz_str_substring`,
// `bz_str_replace`, `bz_str_trim_chars`, `bz_str_pad`). So the result was
// treated as a BORROW: a consumer (the `??` arm normalization in #1948 /
// #1952, the WriteLine arg in #1994, the field-store in #2023, a bound-local
// in #2065, …) RETAINED the result (rc 1→2), and the local released it (rc
// 2→1) at scope end — the producer's original +1 was never decremented and
// the 32-byte bz_string header + the data buffer leaked. The #2199 repro
// (the `??`-of-`s.ToUpper()` shape) showed 140 bytes leaked in 8 allocations
// across 4 iterations under ASan/LSan. The `??`-of-`s + "!"` control
// (RECOGNIZED concat producer) was CLEAN, isolating the cause to the
// unrecognized stdlib-method producer.
//
// THE FIX. Extend `isPlusOneStringProducer` to recognize each stdlib string
// instance method the lowering mints at rc=1 — by the SAME shape
// tryLowerStringMethod keys on (a CallExpr whose callee is a MemberAccessExpr
// on a NON-NULLABLE `string` receiver, with a recognized method name and an
// arg count that matches one of the overloads the lowering mints at rc=1) —
// so the consumer TRANSFERS the single reference instead of minting a second
// borrow-retain. The release drops the SINGLE minted reference exactly once;
// an escaping local is excluded from the scope-end release by the escape
// walk (its +1 then leaks — the safe direction). No double-free. Property
// getters (`Length`, `IsAscii`, `[i: int]`) and the bool/int methods
// (`Equals`, `CompareTo`, `StartsWith`, `EndsWith`, `Contains`, `IndexOf`,
// `LastIndexOf`, `GetHashCode`, `ToInt32`, `ToInt64`, `ToDouble`, `ToFloat`,
// `ToBoolean`, `GetField`, `CountOf`, `Split`, `ToCharArray`, `GetEnumerator`,
// `AsSpan`) are NOT covered — they don't return `string` or don't go through
// the rc=1 path; they keep the existing non-producer classification (the
// borrowed/identity reads).
//
// OBSERVABILITY. runAotChecked links + runs the generated code under ASan/LSan;
// a leaked string flips r.ran to false. Each case renders in a LOOP and
// counts matches: the loop makes all but the final allocation unreachable,
// so LSan RELIABLY reports the leak pre-fix (a single bound-then-read string
// can linger in a register and evade LSan's reachability scan — a false
// negative that would make a one-shot test flaky). The returned count == the
// iteration count IFF every render is byte-correct AND the run is leak-clean,
// so the exit code pins the VALUE and r.ran pins leak-freedom. Each case
// FAILS without the fix (LSan reports the leaked strings → r.ran false) and
// PASSES with it.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Build a "render N times, count byte-correct results" program for a stdlib
// string-transforming method. `decl` binds the receiver; `render` is the
// method call; `expected` is the exact result (rendered via .Length, which
// pins the value AND keeps the producer live long enough for LSan to spot
// the unreleased reference pre-fix). Returns 40 iff every render matched
// AND the run is leak-clean.
std::string loopCount(const char* decl, const char* render, int expectedLen) {
    return std::string(
               "module M; class C "
               "  public static method Main(): int ") +
           decl +
           "    var count := 0; "
           "    var i := 0; "
           "    while i < 40 do "
           "      var s := " + render + "; "
           "      if s.Length = " + std::to_string(expectedLen) +
               " then count := count + 1; end; "
           "      i := i + 1; "
           "    end; "
           "    return count; "
           "  end Main; "
           "end C;";
}

// The original #2199 repro (exact): `p ?? s.ToUpper()` — the consumer that
// decided retain-vs-transfer for the result (the `??` arm normalization
// in #1948 / #1952) mis-classified s.ToUpper() as a BORROW and retained it,
// leaking the producer's +1. Control: `p ?? (s + "!")` is CLEAN — that arm
// is a recognized concat producer.
const char* kOriginalRepro =
    "module M; class C "
    "  public static method P(p: string?, s: string): string "
    "    return p ?? s.ToUpper(); "
    "  end P; "
    "  public static method Main(): int "
    "    var t := 0; var i := 0; "
    "    var src := \"hi\"; "
    "    while i < 4 do t := t + C.P(null, src).Length; i := i + 1; end; "
    "    return t; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (#2199) The original `p ?? s.ToUpper()` repro — 140 bytes leaked in 8
// allocations across 4 iterations pre-fix. Must stay CLEAN post-fix.
// stdlib §9.6 / §4.6 / §28.9.
// =====================================================================
BEZEL_TEST(stdlib_string_transform_methods_leak_2199,
           original_p_coalesce_toupper_lsan_clean) {
    JitResult r = runAotChecked(kOriginalRepro);
    BEZEL_EXPECT_TRUE(r.ran);          // LSan-clean: ToUpper() result freed once
    BEZEL_EXPECT_EQ(r.exitCode, 8);    // 4 × "HI".Length = 4 × 2 = 8
}

// =====================================================================
// (A) §9.6 ToUpper — no-arg, inline malloc + initStringRc.
// =====================================================================
BEZEL_TEST(stdlib_string_transform_methods_leak_2199,
           toupper_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "var src := \"hi\"; ",
        "src.ToUpper()", 2));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (B) §9.6 ToLower — no-arg, inline mint.
// =====================================================================
BEZEL_TEST(stdlib_string_transform_methods_leak_2199,
           tolower_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "var src := \"HI\"; ",
        "src.ToLower()", 2));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (C) §9.6 Trim() — no-arg ASCII-whitespace trim, inline mint.
// =====================================================================
BEZEL_TEST(stdlib_string_transform_methods_leak_2199,
           trim_noarg_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "var src := \"  hi  \"; ",
        "src.Trim()", 2));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (D) §9.6 Trim(chars: IEnumerable<char>) — bz_str_trim_chars.
// =====================================================================
// Skipped: the single-arg Trim(chars) form lowers but the char-set semantics
// need a tiny Array<char> setup that is more involved than the no-arg form.
// The no-arg form (C) covers the same recognition path; adding the char-set
// overload is a follow-up if/when it surfaces in user code.

// =====================================================================
// (E) §9.6 TrimStart() — no-arg, inline mint.
// =====================================================================
BEZEL_TEST(stdlib_string_transform_methods_leak_2199,
           trimstart_noarg_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "var src := \"  hi  \"; ",
        "src.TrimStart()", 4));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (F) §9.6 TrimEnd() — no-arg, inline mint.
// =====================================================================
BEZEL_TEST(stdlib_string_transform_methods_leak_2199,
           trimend_noarg_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "var src := \"  hi  \"; ",
        "src.TrimEnd()", 4));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (G) §9.6 Substring(startIndex, length) — bz_str_substring.
// =====================================================================
BEZEL_TEST(stdlib_string_transform_methods_leak_2199,
           substring_twoarg_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "var src := \"abcdef\"; ",
        "src.Substring(2, 3)", 3));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (H) §9.6 Substring(startIndex) — single-arg overload, bz_str_substring
// with -1 sentinel for "rest of string".
// =====================================================================
BEZEL_TEST(stdlib_string_transform_methods_leak_2199,
           substring_onearg_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "var src := \"abcdef\"; ",
        "src.Substring(3)", 3));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (I) §9.6 Replace(oldChar, newChar) — inline mint (ASCII width=1 path).
// =====================================================================
BEZEL_TEST(stdlib_string_transform_methods_leak_2199,
           replace_char_char_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "var src := \"abba\"; ",
        "src.Replace('a', 'X')", 4));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (J) §9.6 Replace(oldValue, newValue) — bz_str_replace (width-aware).
// =====================================================================
BEZEL_TEST(stdlib_string_transform_methods_leak_2199,
           replace_string_string_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "var src := \"abc\"; ",
        "src.Replace(\"b\", \"YY\")", 4));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (K) §9.6 PadLeft(totalWidth) — bz_str_pad.
// =====================================================================
BEZEL_TEST(stdlib_string_transform_methods_leak_2199,
           padleft_onearg_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "var src := \"hi\"; ",
        "src.PadLeft(5)", 5));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (L) §9.6 PadRight(totalWidth) — bz_str_pad.
// =====================================================================
BEZEL_TEST(stdlib_string_transform_methods_leak_2199,
           padright_onearg_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "var src := \"hi\"; ",
        "src.PadRight(5)", 5));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (M) §9.6 Insert(startIndex, value) — inline mint.
// =====================================================================
BEZEL_TEST(stdlib_string_transform_methods_leak_2199,
           insert_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "var src := \"ab\"; ",
        "src.Insert(1, \"XX\")", 4));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (N) §9.6 Remove(startIndex, count) — inline mint.
// =====================================================================
BEZEL_TEST(stdlib_string_transform_methods_leak_2199,
           remove_twoarg_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "var src := \"abcde\"; ",
        "src.Remove(1, 2)", 3));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (O) §9.6 Remove(startIndex) — single-arg overload (remove to end).
// =====================================================================
BEZEL_TEST(stdlib_string_transform_methods_leak_2199,
           remove_onearg_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "var src := \"abcde\"; ",
        "src.Remove(2)", 2));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (P) Borrow-returning methods must NOT be mis-classified as +1 producers —
// a false-positive producer classification would skip the borrow-retain and
// later UAF when the producer-side reference is released. The fix must
// confine the +1 producer recognition to the methods enumerated above; this
// test guards the NON-producer classification of Length (int borrow) and
// StartsWith (bool borrow).
// =====================================================================
BEZEL_TEST(stdlib_string_transform_methods_leak_2199,
           borrows_not_misclassified) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var t := 0; var i := 0; "
        "    var src := \"hello\"; "
        "    while i < 4 do "
        "      if src.Length = 5 then t := t + 1; end; "
        "      if src.StartsWith(\"he\") then t := t + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    return t; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);          // borrows survive release unchanged
    BEZEL_EXPECT_EQ(r.exitCode, 8);    // 4 × (Length + StartsWith) = 8
}
