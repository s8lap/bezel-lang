// Codegen / leak — the NULLABLE sibling of #1934/#1948: an expression-position
// `case`/`when`, an if-expression, or a `??` whose result type is `string?` and
// whose selected arm MINTS a fresh owned `string?` (`1 => "a"+"b"`, `_ => null`)
// leaked the minted string when the result was bound to a local. Issue #1952.
//
// Spec: §19.2 (case/when arm-meet), §20 (if-expression), §7.7 (`??`), §28.3
// Producers (all three forms are ONE arm-meet rule; the `null`-literal arm
// "absorbs to the other arm's state (K7.5)"; a `(if c then s else null)` "has
// type string?; its ownership is §28.3's null-arm absorption"), §28.9
// (runtime-managed `string` refcount; retain/release are null/sentinel no-ops).
// Spec revision v0.220.
//
// THE BUG. #1934 (and its #1948 siblings) deliberately gated BOTH halves — the
// per-arm +1 normalization in lowerCaseExpr/lowerConditional/lowerCoalesce AND
// the `isPlusOneStringProducer` recognition — on `!nullable`, leaving `string?`
// on the old path. A nullable-`string?` arm-meet result whose producer arm mints
// `"a"+"b"` was therefore neither classified a PRODUCER (the `!nullable` gate)
// nor a BORROW (collectStringReleaseEligible's borrow branch rejects `string?`),
// so a `var s: string? := (case … => "a"+"b", …)` never released the minted +1 —
// it leaked once per binding (LSan: 32-byte header + 3-byte buffer). A nullable
// `var s: string? := "a"+"b"` (a plain concat) was ALREADY clean, because the
// `+` producer row never gated on nullable — proving the gap is specific to the
// gated arm-meet forms, and the fix is exactly to un-gate them.
//
// THE FIX (three parts). (1) `isPlusOneStringProducer` recognizes a `string?`
// case/if arm-meet as a producer — but GUARDED: only when every arm is already
// +1-or-null WITHOUT a per-arm borrow retain (a +1 producer, a `null` literal, a
// string literal, or a diverging `throw`). Then the merged result is uniformly
// {+1 | null | sentinel}, so a `string?` producer local bound to it is released
// soundly at scope end (a null/sentinel release is a no-op). (2) `typeCase`
// applies the §20 null-arm rule (a bare `null` arm makes the result the nullable
// form of the other arms' common type) — without it the case-expr was typed
// `Object?` instead of `string?`, defeating (1). (3) `lowerCaseExpr` Path B no
// longer wraps a `string?` (reference) result into a `{i8, ptr}` value struct —
// it reconciles only a genuine non-pointer arm — so the inferred-type local path
// keeps a bare `ptr` and refcounts it.
//
// WHY GUARDED (the reworked scope). Minting +1 for a BORROW arm of a `string?`
// arm-meet is UNSOUND when the result flows through the un-refcounted nullable
// return path (a `string?` method return does not release the minted +1 —
// #2196): a returned `(if c then p else null)` / `a ?? b` over `string?` borrows
// leaked the retained borrow. The guard confines the fix to arm-meets that need
// NO borrow retain (producer/null/literal arms), which are exactly #1952's
// titled shape. A `string?` `??` is left un-recognized entirely (non-nullable
// only); a `string?` arm-meet with a borrow arm stays on the old (borrow,
// released-by-the-source) path — safe, and regression-free vs the un-refcounted
// nullable return (guarded by tests F/G below).
//
// SCOPE NOTE (recorded). This closes the DIRECT-binding form (the arm-meet
// expression bound to a local in the same body). The ISSUE's own probe routed
// the arm-meet through a `string?` METHOD RETURN (`Pick(): string?` bound in the
// caller); that leak is a DIFFERENT, broader gap — nullable-`string` method
// returns are NOT +1-normalized (the ReturnStmt mint and isUserMethodString
// ReturnCall are both `!nullable`-gated), so `var s := Pick()` leaks even for a
// plain `return "a"+"b"` with no arm-meet at all. Filed separately (#2196); this
// test pins the arm-meet-specific nullable leak #1952 is titled for.
//
// OBSERVABILITY. runAotChecked links + runs under ASan/LSan. Each case binds the
// arm-meet result INSIDE A LOOP so N iterations leak N strings (unmistakable —
// not a single reachability false-negative). The exit code pins the value.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 9 checked rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics, and then RETURNED from the row on
// it. That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a silent
// vacuous pass: on a box whose clang cannot link the ASan binary, all
// 9 rows returned WITHOUT asserting and the suite reported a clean PASS
// having compiled and run nothing. MEASURED on the pre-change tree with the
// harness's own knob (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): every row green.
//
// Every one of those guards becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position: the
// skip is still recognised as an environment condition, but it now MARKS THE
// ROW FAILED instead of hiding it. The lenient legs' assertions additionally
// now RUN UNCONDITIONALLY rather than being skipped, which is the other half of
// the strengthening. Every substantive assertion in every row (r.ran and the
// exitCode pins) stays exactly where it was.

// (A) #1952 repro — a NULLABLE `case` expression whose producer arm mints
// `"a"+"b"`, bound to a `string?` local inside a loop. Each of the 5 iterations
// minted "ab" (the scrutinee C.Sel(1) selects arm 1); pre-fix all 5 leaked.
// total = 5 * "ab".Length = 10.
const char* kNullableCaseProducerArmLoop =
    "module M; class C "
    "  public static method Sel(n: int): int "
    "    return n; "
    "  end Sel; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      var s: string? := (case C.Sel(1) of 1 => \"a\" + \"b\", _ => null); "
    "      total := total + s!.Length; "   // 2 each — value pin + borrow read
    "      i := i + 1; "
    "    end; "
    "    return total; "                    // 10
    "  end Main; "
    "end C;";

// (B) the NULLABLE if-expression sibling (§20), same loop shape.
const char* kNullableIfExprProducerBranchLoop =
    "module M; class C "
    "  public static method Sel(n: int): int "
    "    return n; "
    "  end Sel; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      var s: string? := (if C.Sel(1) = 1 then \"a\" + \"b\" else null); "
    "      total := total + s!.Length; "
    "      i := i + 1; "
    "    end; "
    "    return total; "                    // 10
    "  end Main; "
    "end C;";

// (C) the NULL arm actually taken — the arm-meet result is null. Proves the
// null-arm path is sound (no retain/release of a non-null value, no crash): the
// null branch contributes null, and the scope-end release is a null no-op. s is
// null, so we count nothing; loop returns 0.
const char* kNullableCaseNullArmLoop =
    "module M; class C "
    "  public static method Sel(n: int): int "
    "    return n; "
    "  end Sel; "
    "  public static method Main(): int "
    "    var count := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      var s: string? := (case C.Sel(2) of 1 => \"a\" + \"b\", _ => null); "
    "      if s = null then count := count + 1; end; "
    "      i := i + 1; "
    "    end; "
    "    return count; "                    // 5 (every iteration null)
    "  end Main; "
    "end C;";

// (E) the INFERRED-TYPE form (no `: string?` annotation): `var s := (case …
// _ => null)`. §19.2's null-arm rule types the case `string?`; lowerCaseExpr's
// Path B previously wrapped the `string?` result (a bare `ptr`) into a `{i8,
// ptr}` tagged struct because the non-null `"a"+"b"` arm was misclassified as a
// "non-ptr value" needing reconciliation. The declared-type local path unwrapped
// that struct, but the inferred-type (plain `var`) path did not, leaving the
// minted string un-refcounted → leak. Fixed by only reconciling a GENUINE
// non-pointer arm (matching lowerConditional). total = 5 * "ab".Length = 10.
const char* kInferredNullableCaseProducerLoop =
    "module M; class C "
    "  public static method Sel(n: int): int "
    "    return n; "
    "  end Sel; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      var s := (case C.Sel(1) of 1 => \"a\" + \"b\", _ => null); "
    "      total := total + s!.Length; "
    "      i := i + 1; "
    "    end; "
    "    return total; "                    // 10
    "  end Main; "
    "end C;";

// (F) REGRESSION GUARD — a NULLABLE arm-meet with a BORROW arm, RETURNED through
// a `string?` method and bound in the caller, must stay LSan-CLEAN. This is the
// shape the FIRST cut of #1952 regressed: minting +1 for the borrow arm (`b`, a
// param) of a `string?` arm-meet leaked when the result flowed through the
// un-refcounted `string?` return path (#2196), where nothing releases the minted
// +1. The rework confines the arm-meet +1 normalization to NON-nullable results
// and recognizes a `string?` arm-meet as a producer ONLY when every arm is
// already +1-or-null (a borrow arm fails that guard), so this shape stays on the
// old (borrow, released-by-the-source) path — clean, no minted +1 to strand.
// `bb` ("bb") is the source; it is released at its own scope end. total =
// 3 * "bb".Length (2) = 6.
const char* kNullableCaseBorrowArmReturnedLoop =
    "module M; class C "
    "  public static method P(n: int, b: string?): string? "
    "    return (case n of 1 => b, _ => null); "
    "  end P; "
    "  public static method Main(): int "
    "    var total := 0; var i := 0; "
    "    while i < 3 do "
    "      var bb: string? := \"b\" + \"b\"; "
    "      var r := C.P(1, bb); "
    "      total := total + r!.Length; "
    "      i := i + 1; "
    "    end; "
    "    return total; "                    // 6
    "  end Main; "
    "end C;";

// (G) REGRESSION GUARD — the `??` analog: `return a ?? b` over two `string?`
// borrows, bound in the caller. The first cut of #1952 minted +1 for the `??`
// borrow arms and leaked it through the `string?` return; the rework leaves a
// `string?` `??` un-normalized (non-nullable-only), so it stays clean. `pb`
// ("bb") is the source, released at its own scope end. total = 3 * 2 = 6.
const char* kNullableCoalesceBorrowsReturnedLoop =
    "module M; class C "
    "  public static method P(a: string?, b: string?): string? "
    "    return a ?? b; "
    "  end P; "
    "  public static method Main(): int "
    "    var total := 0; var i := 0; "
    "    while i < 3 do "
    "      var pa: string? := null; "
    "      var pb: string? := \"b\" + \"b\"; "
    "      var r := C.P(pa, pb); "
    "      total := total + r!.Length; "
    "      i := i + 1; "
    "    end; "
    "    return total; "                    // 6
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) #1952 repro — a nullable `case`-expression producer arm bound in a loop
// is LSan-clean. §19.2 / §28.3 (arm-meet, null-arm absorption) / §28.9.
// =====================================================================
BEZEL_TEST(nullable_string_arm_meet_result_leak_1952,
           nullable_case_producer_arm_loop_lsan_clean_spec_19_2) {
    JitResult r = runAotChecked(kNullableCaseProducerArmLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);          // 5 minted strings each freed once
    BEZEL_EXPECT_EQ(r.exitCode, 10);   // 5 * "ab".Length
}

// =====================================================================
// (B) The nullable if-expression sibling — LSan-clean. §20 / §28.3 / §28.9.
// =====================================================================
BEZEL_TEST(nullable_string_arm_meet_result_leak_1952,
           nullable_if_expr_producer_branch_loop_lsan_clean_spec_20) {
    JitResult r = runAotChecked(kNullableIfExprProducerBranchLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// =====================================================================
// (C) The null arm taken — sound (no crash, no spurious retain/release).
// §28.3 null-arm absorption (K7.5) / §28.9.
// =====================================================================
BEZEL_TEST(nullable_string_arm_meet_result_leak_1952,
           nullable_case_null_arm_loop_sound_spec_28_3) {
    JitResult r = runAotChecked(kNullableCaseNullArmLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// =====================================================================
// (E) The INFERRED-type form — no `: string?` annotation. Exercises the
// lowerCaseExpr Path B reference-vs-value reconciliation fix. §19.2 / §7.1.
// =====================================================================
BEZEL_TEST(nullable_string_arm_meet_result_leak_1952,
           inferred_nullable_case_producer_loop_lsan_clean_spec_19_2) {
    JitResult r = runAotChecked(kInferredNullableCaseProducerLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// =====================================================================
// (F) REGRESSION GUARD — a nullable arm-meet with a BORROW arm, returned through
// a `string?` method, is LSan-clean (no minted +1 stranded on the un-refcounted
// nullable return path — #2196). §28.3 / §28.9.
// =====================================================================
BEZEL_TEST(nullable_string_arm_meet_result_leak_1952,
           nullable_case_borrow_arm_returned_loop_no_regression_spec_28_3) {
    JitResult r = runAotChecked(kNullableCaseBorrowArmReturnedLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// =====================================================================
// (G) REGRESSION GUARD — the `??` analog: `return a ?? b` over two `string?`
// borrows is LSan-clean (a `string?` `??` is left un-normalized). §7.7 / §28.9.
// =====================================================================
BEZEL_TEST(nullable_string_arm_meet_result_leak_1952,
           nullable_coalesce_borrows_returned_loop_no_regression_spec_7_7) {
    JitResult r = runAotChecked(kNullableCoalesceBorrowsReturnedLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// =====================================================================
// (H) fix #2196 / §28.9 — the issue's own repro: a `string?` method return
// whose BODY hands the caller a fresh +1 string (a `+` concat, no arm-meet
// involved) leaks that string when the caller binds the result to a local.
// The non-nullable sibling (`Pick(): string` → "a"+"b" → bound) is clean;
// the nullable sibling leaked exactly 35B (32-byte header + 3-byte buffer)
// because the WHOLE nullable-string refcount path was gated `!nullable`:
//   * callee side — ReturnStmt's `+1` mint (LlvmBackend_Stmts.cpp) is gated
//     `!declRet->nullable`, so a `string?` return neither retains a borrow
//     nor is recognized as a transfer — the value just passes through;
//   * caller side — `isUserMethodStringReturnCall` (LlvmBackend_Setup.cpp)
//     is gated `!nullable` (both imported and local-method branches), so a
//     `Pick(): string?` call is not classified a +1 producer; the caller's
//     `var s` then falls to `collectStringReleaseEligible`'s BORROW branch,
//     which ALSO rejects `string?` — so the local is tracked for neither
//     transfer+release nor retain+release, and its +1 leaks.
// Fix #2196 un-gates BOTH halves (in lockstep — extending caller-side
// recognition without the callee-side mint is a UAF, the §28.9 residue-1
// hazard the comment at #694 flagged): ReturnStmt mints +1 for a `string?`
// return (null-safe: `bz_string_retain` is a no-op on a sentinel/null), and
// `isUserMethodStringReturnCall` recognizes the `string?` return as a +1
// producer. `var s := C.Pick(1)` is now a transfer+release (no leak), and
// the loop's 5 iterations do not accumulate. §4.6 / §28.3 / §28.9.
// =====================================================================
const char* kNullableStringMethodReturnBindLoop =
    "module M; class C "
    "  public static method Pick(n: int): string? "
    "    return \"a\" + \"b\"; "
    "  end Pick; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      var s := C.Pick(1); "
    "      total := total + s!.Length; "
    "      i := i + 1; "
    "    end; "
    "    return total; "                    // 5 * "ab".Length (2) = 10
    "  end Main; "
    "end C;";

BEZEL_TEST(nullable_string_arm_meet_result_leak_1952,
           nullable_string_method_return_bind_loop_lsan_clean_28_9) {
    JitResult r = runAotChecked(kNullableStringMethodReturnBindLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);         // 5 minted strings each freed once
    BEZEL_EXPECT_EQ(r.exitCode, 10);  // 5 * "ab".Length
}

// =====================================================================
// (I) fix #2200 / §28.3 / §28.9 — nullable CONDITIONAL-ESCAPE: a `string`
// producer LOCAL escaping via one arm of a `string?` `case`/`if` leaked on
// the non-escaping paths. The non-nullable sibling was fixed by #1951
// (retain a bare string-local arm read; the StringEscapeWalker relaxes the
// local to scope-end-release eligible; balanced on every path). The
// nullable analog was deliberately deferred by #1952 because the nullable
// return path was un-refcounted (#2196 root). After #2196 the whole
// `string?` value path is refcounted, so extending both halves (the
// per-arm mint in lowerCaseExpr/lowerConditional AND the
// StringEscapeWalker.isStringResultArmMeet relaxation) to nullable closes
// the leak. The issue's exact repro: `var s := "x" + "y"; return (case n
// of 1 => s, _ => null);` — n=2 always takes the null arm; pre-fix `s`'s
// +1 leaked each call (105B = 32B header * 3 + 9B indirect buffer).
// // per #1951, #2196, #2200, §28.3, §28.9
// =====================================================================
const char* kNullableConditionalEscapeLocalLoop =
    "module M; class C "
    "  public static method P(n: int): string? "
    "    var s := \"x\" + \"y\"; "
    "    return (case n of 1 => s, _ => null); "
    "  end P; "
    "  public static method Main(): int "
    "    var t := 0; var i := 0; "
    "    while i < 4 do "
    "      var r := C.P(2); "
    "      if r = null then t := t + 1; end; "
    "      i := i + 1; "
    "    end; "
    "    return t; "  // 4 — every iteration's null arm
    "  end Main; "
    "end C;";

BEZEL_TEST(nullable_string_arm_meet_result_leak_1952,
           nullable_conditional_escape_local_lsan_clean_spec_28_3) {
    JitResult r = runAotChecked(kNullableConditionalEscapeLocalLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);         // 4 minted "xy" each freed once
    BEZEL_EXPECT_EQ(r.exitCode, 4);   // 4 (every iteration null)
}

// The if-expression sibling of #2200. Same shape, `if` not `case`.
// §20 / §28.3 / §28.9.
const char* kNullableIfConditionalEscapeLocalLoop =
    "module M; class C "
    "  public static method P(n: int): string? "
    "    var s := \"x\" + \"y\"; "
    "    return (if n = 1 then s else null); "
    "  end P; "
    "  public static method Main(): int "
    "    var t := 0; var i := 0; "
    "    while i < 4 do "
    "      var r := C.P(2); "
    "      if r = null then t := t + 1; end; "
    "      i := i + 1; "
    "    end; "
    "    return t; "  // 4
    "  end Main; "
    "end C;";

BEZEL_TEST(nullable_string_arm_meet_result_leak_1952,
           nullable_if_conditional_escape_local_lsan_clean_spec_28_3) {
    JitResult r = runAotChecked(kNullableIfConditionalEscapeLocalLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}
