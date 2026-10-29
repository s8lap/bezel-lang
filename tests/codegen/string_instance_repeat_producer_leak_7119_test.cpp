// PORTED by fix #7706 (stdlib §9.6, v0.350). This file pinned the INSTANCE
// `"x".Repeat(count)` spelling's +1 producer accounting. §9.6 declares
// `Repeat` only as the two STATIC members, so the instance spelling is now
// rejected (tests/sema/string_instance_repeat_static_only_7706_test.cpp) and
// every row below uses the declared static `String.Repeat(unit, count)` --
// the same leak-freedom property (a fresh +1 string released by its consumer)
// on the spelling that exists. The test names keep "instance_repeat" so the
// history of the rows stays greppable.
// Codegen / leak — the INSTANCE `string` producer recognizer missed the
// 1-argument `Repeat` (and two siblings), so each result's producer
// reference was never released. Issue #7119.
//
// Spec: language spec §4.6 (`string` is runtime-managed; `s[r]` returns "a
// fresh `string`"), §28.9 (runtime-managed types), stdlib §9.6 (the `string`
// class: `Repeat(c: char, count: int): string`, the static
// `Repeat(unit: string, count: int): string`, and the slicing property
// `public pure readonly property [r: Range<int>]: string;`). Spec revision
// v0.343.
//
// THE BUG. `Emitter::isPlusOneStringProducer` (src/codegen/LlvmBackend_Setup.cpp)
// decides whether a `string`-typed expression hands the consumer a fresh
// owned reference (+1) or a borrow (+0). Its static-factory arm recognized
// `string.Repeat(char, count)` (2 args) and its instance arm enumerated the
// §9.6 transforms (Trim / ToUpper / ToLower / Substring / Replace /
// PadLeft / PadRight / Insert / Remove). Three members that DO mint a fresh
// +1 string were absent from both arms:
//
//   * `Repeat(count)` — the 1-argument INSTANCE spelling ("x".Repeat(200)).
//     Its lowering (tryLowerStringMethod, src/codegen/LlvmBackend_String.cpp)
//     does an inline `malloc` + `initStringRc` (rc := 1). MEASURED before the
//     fix: 233 B in 2 allocations for `"x".Repeat(200)`, and 283 B for the
//     receiver the SHIPPED #6641 test builds — `"c" + "x".Repeat(250) + "c"`.
//   * `GetField(delim, n)` — the §9.6-ext member returning the n-th field of
//     SELF split by `delim`; its lowering ends in the same `initStringRc`.
//     MEASURED before the fix: 34 B in 2 allocations.
//   * `s[r]` where `r` is a Range-typed VALUE (a variable / call result) — the
//     second spelling of the §9.6 slicing property
//     `[r: Range<int>]: string`. The indexer lowering recognizes BOTH
//     spellings (a RangeExpr literal `s[0..2]` and a Range-typed value `s[r]`)
//     and funnels them through `emitStringSliceBytes`, which mints rc := 1 —
//     but the recognizer matched only the literal. MEASURED before the fix:
//     36 B in 2 allocations for one slice.
//
// So the consumer classified each result as a BORROW: it RETAINED the value
// (rc 1 -> 2) and the local released it at scope end (rc 2 -> 1), and the
// producer's original +1 was never dropped — the 32-byte bz_string header and
// its data buffer leaked.
//
// THE FIX. Teach the recognizer the three shapes, each keyed on the SAME
// condition the lowering keys on — the member name, the argument count, and
// the receiver's type — so the recognizer and the lowering stay in lockstep:
//   * the instance arm gains `Repeat` with argc == 1 (the 2-argument STATIC
//     spelling is the static arm's, and was already there);
//   * the instance arm gains `GetField` with argc == 2;
//   * the IndexExpr arm accepts a Range-typed index, not only a RangeExpr
//     literal.
// The consumer then TRANSFERS the single reference instead of retaining on top
// of it, and the scope-end release drops it exactly once — no double-free.
//
// OBSERVABILITY. runAotChecked compiles, links and RUNS the generated code
// under ASan/LSan; a leaked string flips r.ran to false. Every case renders
// in a LOOP (40 iterations) and counts byte-correct results: the loop makes
// all but the final allocation unreachable, so LSan RELIABLY reports the leak
// pre-fix (a single bound-then-read string can linger in a register and
// evade LSan's reachability scan — a false negative that would make a
// one-shot test flaky). The returned count == the iteration count IFF every
// render is byte-correct AND the run is leak-clean, so the exit code pins the
// VALUE and r.ran pins leak-freedom. Each case FAILS without the fix (LSan
// reports the leaked strings -> r.ran false) and PASSES with it.
//
// NOT IN SCOPE. The negative-count contract of `Repeat` (stdlib §9.6: "count
// < 0 raises ArgumentOutOfRangeException") is a SEPARATE defect — the
// instance arm has no guard on the count and the static arm clamps to 0 —
// tracked by #7120. This file does not assert it.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Build a "render N times, count byte-correct results" program. `decl` binds
// the receiver(s); `render` is the expression under test; `expectedLen` is the
// exact length the render must have. Returns 40 iff every render matched AND
// the run is leak-clean.
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

}  // namespace

// =====================================================================
// (A) The #7119 repro, exactly: `var s: string := "x".Repeat(200);`.
// Pre-fix: 233 B in 2 allocations (32-byte header + 201-byte data).
// stdlib §9.6 / §4.6 / §28.9.
// =====================================================================
BEZEL_TEST(string_instance_repeat_producer_leak_7119,
           instance_repeat_literal_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "",
        "String.Repeat(\"x\", 200)", 200));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // LSan-clean: the producer's +1 released
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (B) The same member with the count in a variable — the receiver shape the
// SHIPPED #6641 test builds is `"c" + "x".Repeat(250) + "c"` (283 B pre-fix).
// The concat consumer is a recognized producer too, so the whole expression
// is one +1 chain.
// =====================================================================
BEZEL_TEST(string_instance_repeat_producer_leak_7119,
           instance_repeat_count_in_variable_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "var n := 200; ",
        "String.Repeat(\"x\", n)", 200));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (C) The receiver of the shipped tests/codegen/
// string_indexof_startindex_range_6641_test.cpp: `"c" + "x".Repeat(250) + "c"`
// — 252 code points. Pre-fix: 283 B in 2 allocations.
// =====================================================================
BEZEL_TEST(string_instance_repeat_producer_leak_7119,
           instance_repeat_in_concat_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "",
        "\"c\" + String.Repeat(\"x\", 250) + \"c\"", 252));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (D) The null-forgiving spelling `s!.Repeat(50)` — the receiver is
// narrowed to a non-nullable `string` by `!`, so the instance arm's
// receiver condition holds. Pre-fix: 83 B in 2 allocations.
// =====================================================================
BEZEL_TEST(string_instance_repeat_producer_leak_7119,
           instance_repeat_null_forgiving_receiver_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "var src: string? := \"x\"; ",
        "String.Repeat(src!, 50)", 50));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (E) The `Console.WriteLine` argument consumer (#1994's shape): the
// argument itself is the producer, and the callee is its only owner.
// Pre-fix: 83 B in 2 allocations per call.
// =====================================================================
BEZEL_TEST(string_instance_repeat_producer_leak_7119,
           instance_repeat_write_arg_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 40 do "
        "      Console.WriteLine(String.Repeat(\"x\", 50)); "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// (F) The return-handoff consumer: a method returns the producer, the
// caller binds it and releases it at scope end.
// =====================================================================
BEZEL_TEST(string_instance_repeat_producer_leak_7119,
           instance_repeat_returned_from_method_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Make(): string "
        "    return String.Repeat(\"x\", 50); "
        "  end Make; "
        "  public static method Main(): int "
        "    var count := 0; "
        "    var i := 0; "
        "    while i < 40 do "
        "      var s := C.Make(); "
        "      if s.Length = 50 then count := count + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (G) The `??` consumer (#1948 / #1952's arm normalization). The count is
// kept <= 255 so the exit code is not truncated by the 8-bit exit status.
// =====================================================================
BEZEL_TEST(string_instance_repeat_producer_leak_7119,
           instance_repeat_coalesced_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method P(p: string?, s: string): string "
        "    return p ?? String.Repeat(s, 2); "
        "  end P; "
        "  public static method Main(): int "
        "    var count := 0; "
        "    var i := 0; "
        "    var src := \"hi\"; "
        "    while i < 40 do "
        "      if C.P(null, src).Length = 4 then count := count + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// (H) removed by fix #7746 (stdlib §9.6, v0.350): the instance `GetField`
// is not a §9.6 member and is rejected, so it has no producer to leak.

// =====================================================================
// (I) The slicing property `[r: Range<int>]: string` in its Range-VALUE
// spelling: `s[r]` where `r` is a Range-typed VARIABLE, not a literal.
// Language spec §4.6: "`s[r]` for a `Range<int>` value `r` returns the
// substring spanning `r` ... The result is a fresh `string`".
// Pre-fix: 36 B in 2 allocations.
// =====================================================================
BEZEL_TEST(string_instance_repeat_producer_leak_7119,
           slice_range_value_lsan_clean_spec_4_6) {
    JitResult r = runAotChecked(loopCount(
        "var r: Range<int> := 0..2; ",
        "\"abcdef\"[r]", 3));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (J) Two slices in one scope — the second slice must not inherit the
// first one's classification. Pre-fix: 72 B in 4 allocations.
// =====================================================================
BEZEL_TEST(string_instance_repeat_producer_leak_7119,
           slice_range_value_twice_lsan_clean_spec_4_6) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var r: Range<int> := 0..2; "
        "    var count := 0; "
        "    var i := 0; "
        "    while i < 40 do "
        "      var a := \"abcdef\"[r]; "
        "      var b := \"ghijkl\"[r]; "
        "      if a.Length + b.Length = 6 then count := count + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (K) CONTROL — the STATIC spelling `string.Repeat(char, count)`. It was
// already recognized before this fix (the static arm), so it is the
// control that isolates the cause to the INSTANCE arm's coverage.
// =====================================================================
BEZEL_TEST(string_instance_repeat_producer_leak_7119,
           static_repeat_control_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "",
        "string.Repeat('x', 200)", 200));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (L) CONTROL — the slicing property in its RangeExpr LITERAL spelling
// `s[0..2]`, which the recognizer already covered. It must stay clean (the
// widened IndexExpr arm must not have broken it).
// =====================================================================
BEZEL_TEST(string_instance_repeat_producer_leak_7119,
           slice_range_literal_control_lsan_clean_spec_4_6) {
    JitResult r = runAotChecked(loopCount(
        "",
        "\"abcdef\"[0..2]", 3));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (M) CONTROL — an already-recognized instance member (§9.6 Trim) and an
// already-recognized producer (the `+` concat), both of which must stay
// clean: the widened recognizer must not have introduced a double-release.
// =====================================================================
BEZEL_TEST(string_instance_repeat_producer_leak_7119,
           trim_and_concat_controls_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "var src := \"  hi  \"; ",
        "src.Trim() + \"!\"", 3));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}
