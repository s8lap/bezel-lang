// Codegen / leak — Issue #1298: "Pad*/string-method temporaries used in a
// compound expression leak (string local not released when it flows into a
// boolean/multi-use expression)". Pins the issue's own repro AND the wider
// "compound / multi-use expression" family its title describes, against a
// regression in either isPlusOneStringProducer's stdlib-method recognition
// (#2199) or StringEscapeWalker's generic BinaryExpr safe-read treatment
// (#422, both in LlvmBackend_Setup.cpp).
//
// Spec: stdlib §9.6 (`string` instance methods PadLeft/PadRight/ToUpper/
// Substring/… each return a fresh `string`), language spec §4.6 (the `string`
// type; runtime-managed refcount discipline), §28.3 (producers table /
// "Unbound owned temporaries"), §28.9 (runtime-managed types — string
// lifecycle, including its refcounting realization, is implementation-
// defined, not spec-mandated: "The implementation of runtime-managed types'
// lifecycle … is not part of this specification"), §20 (the `(if … then …
// else …)` expression atom — the CURRENT spelling; the issue's own repro used
// the postfix `V if C else W` form, RETIRED at v0.208 in favor of the
// parenthesized atom, so the repro is respelled here to compile under the
// current grammar). Spec revision v0.228.
//
// VERIFY-FIRST FINDING (2026-07-29): the issue's exact repro (respelled for
// v0.208+ if-expression syntax) is ALREADY CLEAN on current main — 0 bytes
// leaked, confirmed under LSan both single-shot and in a 40-iteration loop.
// Root-caused by BISECTING WITH A MANUAL REVERT (not archaeology): reverse-
// applying just the `isPlusOneStringProducer` hunk of fix #2199 ("recognize
// stdlib string-transforming methods as +1 producers", landed 2026-07-25,
// AFTER this issue was filed 2026-07-01) reproduces the leak EXACTLY as
// reported — 77 bytes in 4 allocations for the single-shot repro, byte-for-
// byte identical to the issue's own pasted ASan summary — and re-applying the
// hunk restores the clean state. So #2199 already fixed this issue; the
// present change adds the regression test #2199's own suite
// (stdlib_string_transform_methods_leak_2199_test.cpp) did not cover: the
// MULTI-LOCAL and MULTI-USE compound-expression shapes #1298's title
// describes, which are a distinct code path (StringEscapeWalker's generic
// BinaryExpr arm, present since the original #422 string-refcounting
// commit — Emitter::collectStringReleaseEligible / the string-release walk)
// from the single-bind-then-read shapes #2199 already pins.
//
// A CORRECTION to the issue's own diagnosis: its stated control — "A single
// pad returned directly (`var p := "ab".PadLeft(6); return p.Length;`) is
// ASan-clean" — does NOT hold under the pre-#2199 code: manually reverting
// the #2199 hunk and re-testing the single-use shape IN A LOOP (single-shot
// LSan runs can register-hide a not-yet-reclaimed heap string — see
// #2199's OBSERVABILITY note) shows it ALSO leaked (1560 bytes / 80
// allocations over 40 iterations) before #2199. The issue's hypothesized
// root cause — "the #422 release NOT firing when a string local flows into a
// compound/boolean expression" — was therefore not the actual defect; the
// escape walk's generic BinaryExpr treatment of `and`/`or`/comparison
// operands as non-consuming reads (both operands walked with safeRead=true
// unconditionally, regardless of the incoming safeRead/escape context) has
// applied since #422's original 2026-06-05 commit and already handles every
// compound-expression shape correctly. The true defect was the PRODUCER
// classification gap #2199 closed: pre-#2199, `isPlusOneStringProducer`
// excluded PadLeft/PadRight (and every other §9.6 string-transform method),
// so a bound local was mis-routed to the BORROW path — retained an extra
// reference at bind (rc 1->2) that the (already-correct) scope-end release
// only brought back to rc=1, leaking the producer's original +1 — in BOTH
// the single-use and the compound-expression shapes alike; #2199's fix
// (recognizing the methods as +1 producers, so the bind TRANSFERS instead of
// retaining) closed the gap for both simultaneously.
//
// THE FULL SHAPE MATRIX EXERCISED HERE (all LSan-clean on current main):
//   (A) the issue's exact repro: 3 distinct Pad*-bound locals combined in an
//       `and`-chain inside an if-expression condition.
//   (B) single-use control (direct `.Length` read) — the issue's own
//       (inaccurate, see above) "already clean" claim, pinned properly via a
//       LOOP so a false negative cannot hide a regression.
//   (C) concatenation of 3 producer locals (`a + b + c`).
//   (D) the SAME local read 3 times within ONE compound `and`-chain
//       (`.Length`, `.StartsWith`, `.EndsWith`) — the "multi-use" (as
//       opposed to "multi-local") half of the issue title.
//   (E) the SAME local read across 3 SEPARATE statements (not one compound
//       expression) — multi-use without compounding.
//   (F) reassignment (the #1997 relaxation) immediately followed by a
//       compound-expression read of the reassigned value — the adversarial
//       double-free-risk combination of two independently-landed
//       relaxations (#1997 reassignment + the #422 compound-expression
//       safe-read); ASan's own heap-corruption detector (always active,
//       independent of detect_leaks) would catch a double-free here.
//   (G) an `or`-chain (the issue mentions "and-chain, or otherwise
//       multi-consumed" — `or` is mechanically the same BinaryExpr arm, but
//       given its own case for direct traceability to the issue text).
//
// OBSERVABILITY. runAotChecked links + runs under ASan/LSan; a leaked string
// flips r.ran to false. Every case renders in a 40-iteration LOOP (per
// #2199's OBSERVABILITY note: a single bound-then-read string can linger in
// a register and evade LSan's reachability scan on a one-shot run — the
// single-shot original repro is ALSO included as a companion for direct
// byte-count comparison to the issue's pasted ASan summary, but the loop
// cases are the ones that reliably fail pre-fix). Each case FAILS (leak,
// r.ran false) under a manual revert of the #2199 isPlusOneStringProducer
// hunk and PASSES (r.ran true, exact count) on current main — verified
// mechanically (git apply -R / rebuild / confirm fail / git apply / rebuild
// / confirm pass) before this test was added; see the fix-#1298 commit
// message for the exact before/after byte counts per shape.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 8 absorbing rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics and returned from the row on it —
// the definition scripts/check_toolchain_skip_surface.py uses for an
// ABSORBING site. It was also a silent vacuous pass: on a box whose clang
// cannot link the ASan binary, every one of those rows returned WITHOUT
// asserting, and the suite reported a clean PASS having compiled and run
// nothing.
//
// Each guard becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position:
// the skip is now LOUD — the row FAILS — instead of being absorbed by a
// guard that reports a clean pass.
// Every substantive assertion in every row — `r.ran` and the exitCode pins —
// stays exactly where it was, and the local predicate is deleted so no
// absorbing shape survives. The guard stays a SEPARATE statement from the
// predicate on purpose: inlining it into an `if` recreates the shape the
// predicate exists to end.

}  // namespace

// =====================================================================
// (A) The issue's EXACT repro, respelled for the current (v0.208+) §20
// if-expression atom syntax: 3 distinct PadLeft/PadRight-bound locals
// combined with `and` inside an if-expression condition. Looped 40x so LSan
// reliably reports a per-iteration leak (the single-shot companion below
// pins the exact issue-reported byte count).
// stdlib §9.6, §4.6, §20, §28.3, §28.9.
// =====================================================================
BEZEL_TEST(string_producer_compound_expr_leak_1298,
           exact_repro_and_chain_looped_lsan_clean) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var count := 0; "
        "    var i := 0; "
        "    while i < 40 do "
        "      var a := \"ab\".PadLeft(4); "
        "      var b := \"x\".PadRight(5, '*'); "
        "      var c := \"ab\".PadLeft(6); "
        "      if (if (a.Length = 4) and (b.Length = 5) and (c = \"    ab\") "
        "          then 1 else 0) = 1 then count := count + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);          // LSan-clean across 40 iterations
    BEZEL_EXPECT_EQ(r.exitCode, 40);   // every iteration's condition holds
}

// =====================================================================
// (A') Single-shot companion: the issue's exact repro, unlooped, for a
// direct comparison to the issue's own pasted ASan summary ("77 byte(s)
// leaked in 4 allocation(s)"). A manual revert of #2199's
// isPlusOneStringProducer hunk reproduces that EXACT figure (verified before
// writing this test); current main is clean.
// =====================================================================
BEZEL_TEST(string_producer_compound_expr_leak_1298,
           exact_repro_single_shot_lsan_clean) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var a := \"ab\".PadLeft(4); "
        "    var b := \"x\".PadRight(5, '*'); "
        "    var c := \"ab\".PadLeft(6); "
        "    return (if (a.Length = 4) and (b.Length = 5) and "
        "               (c = \"    ab\") then 1 else 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// =====================================================================
// (B) Single-use control, LOOPED (correcting the issue's own single-shot
// claim — see the file header note: this shape ALSO leaked pre-#2199, a
// one-shot LSan false negative masked it). A regression in the producer
// recognition would flip this to leaking too.
// =====================================================================
BEZEL_TEST(string_producer_compound_expr_leak_1298,
           single_use_direct_read_looped_lsan_clean) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var count := 0; "
        "    var i := 0; "
        "    while i < 40 do "
        "      var p := \"ab\".PadLeft(6); "
        "      if p.Length = 6 then count := count + 1; end; "
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
// (C) Concatenation of 3 producer locals (`a + b + c`) — the issue's
// "otherwise multi-consumed" alternative to a boolean chain.
// =====================================================================
BEZEL_TEST(string_producer_compound_expr_leak_1298,
           concat_of_three_producers_lsan_clean) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var count := 0; "
        "    var i := 0; "
        "    while i < 40 do "
        "      var a := \"ab\".PadLeft(4); "
        "      var b := \"x\".PadRight(5, '*'); "
        "      var c := \"ab\".PadLeft(6); "
        "      var joined := a + b + c; "
        "      if joined.Length = 15 then count := count + 1; end; "
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
// (D) The SAME local read 3 times within ONE compound `and`-chain — the
// "multi-use" (not just "multi-local") reading of the issue title.
// =====================================================================
BEZEL_TEST(string_producer_compound_expr_leak_1298,
           same_local_thrice_in_one_compound_lsan_clean) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var count := 0; "
        "    var i := 0; "
        "    while i < 40 do "
        "      var a := \"ab\".PadLeft(4); "
        "      if (a.Length = 4) and (a.StartsWith(\"  \")) and "
        "         (a.EndsWith(\"ab\")) then count := count + 1; end; "
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
// (E) The SAME local read across 3 SEPARATE statements (not one compound
// expression) — multi-use without compounding, the other half of the
// "boolean and-chain, or otherwise multi-consumed" phrasing.
// =====================================================================
BEZEL_TEST(string_producer_compound_expr_leak_1298,
           same_local_across_separate_statements_lsan_clean) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var count := 0; "
        "    var i := 0; "
        "    while i < 40 do "
        "      var a := \"ab\".PadLeft(4); "
        "      if a.Length = 4 then count := count + 1; end; "
        "      if a.StartsWith(\"  \") then count := count + 1; end; "
        "      if a = \"  ab\" then count := count + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 120);  // 3 increments/iteration * 40
}

// =====================================================================
// (F) ADVERSARIAL: reassignment (the #1997 relaxation) immediately followed
// by a compound-expression read of the reassigned value. Combines two
// independently-landed relaxations; if either over-releases the FIRST
// (displaced) value or under-releases the FINAL value, ASan's own
// heap-corruption detector (independent of detect_leaks) or LSan would catch
// it. Must stay double-free-free AND leak-free.
// =====================================================================
BEZEL_TEST(string_producer_compound_expr_leak_1298,
           reassign_then_compound_read_no_double_free_lsan_clean) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var count := 0; "
        "    var i := 0; "
        "    while i < 40 do "
        "      var a := \"zz\".PadLeft(4); "
        "      a := \"ab\".PadLeft(4); "
        "      if (a.Length = 4) and (a.StartsWith(\"  \")) and "
        "         (a = \"  ab\") then count := count + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);          // no double-free, no leak
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (G) An `or`-chain — the issue text's "and-chain, or otherwise
// multi-consumed"; mechanically the same BinaryExpr arm as `and`, given its
// own case for direct traceability.
// =====================================================================
BEZEL_TEST(string_producer_compound_expr_leak_1298,
           or_chain_lsan_clean) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var count := 0; "
        "    var i := 0; "
        "    while i < 40 do "
        "      var a := \"ab\".PadLeft(4); "
        "      if (a.Length = 999) or (a = \"  ab\") then "
        "        count := count + 1; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}
