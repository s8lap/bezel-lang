// Codegen — the two §8.6 box-mint residuals filed against the parent #2876
// fix (commit `446b3aa`, which closed the call-argument and interpolation-hole
// positions for a `boxReturnMethods_` call, but did NOT cover these two
// surfaces):
//
//   #3351: an Object box a `boxReturnMethods_` callee mints still leaks when
//   the consumer binds it THROUGH `as Object/interface`
//   (`var ob: Object := h.M() as Object;`). §17.4's `as`-to-a-reference-target
//   arm RETURNS the operand (the box) — the cast hand-off does not transfer
//   ownership; the receiver IS the box, the bound local is the +1 owner.
//   `boxLocalAssignmentSourceKind` (LlvmBackend_Setup.cpp) tested case (a)
//   "static type is boxable" against the cast's type (`Object`, not boxable)
//   and case (b) "initializer is a `CallExpr` in `boxReturnMethods_`" against
//   the cast (`TypeTestExpr`, not `CallExpr`), so `isPlusOneBoxProducer`
//   answered false, the local was absent from `boxReleaseEligible_`, and the
//   box the callee minted leaked at scope end. Measured 480 B / 20 allocations
//   in a 20-iteration loop for the issue's repro.
//
//   #3352: an INLINE §8.6 box mint at a call-arg or §18.5 interpolation-hole
//   position — the indexer/property inline-body path
//   (`lowerIndexerGet` line 2141 / `lowerVirtualPropertyGet`'s
//   `boxForTarget` ~line 5505) mints a fresh box for a value-typed body
//   delivered where `Object` is expected, records the mint in
//   `lastBoxMint_`, and the box leaks because the only existing reader of
//   `lastBoxMint_` is `lowerTypeTest` (the `is`/`is not`/`as <value type>`
//   shape, #2865). The CALL-ARG position (`Console.WriteLine(e[k])`) and
//   the §18.5 INTERPOLATION-HOLE position (`$"[{e[k()}]"`) both leave the
//   inline mint unreclaimed. Measured 480 B / 20 allocations for the
//   issue's repro.
//
// Spec (revision v0.240):
//   §8.6 "Boxing" — the wrapper is +1 the callee transfers to the caller.
//     Both inline mints (the indexer/property expression-body `boxForTarget`)
//     and cross-frame mints (a `boxReturnMethods_` callee) follow the same
//     §8.6 ownership contract; only the call/return edge differs.
//   §17.4 "`is` / `is not` / `as`" — the `as Object` arm RETURNS the operand.
//   §23.4 "Indexers" — expression-bodied indexer body inline-lowered, the
//     site of the inline mint.
//   §28.3 "Local variables, ownership, and borrows" — bound local owns the +1
//     and discharges at scope end (#3351); unbound temporary at a call-arg /
//     interpolation hole is the §28.3 unbound-owned-temporary rule
//     (#3352).
//   §28.9 "Runtime-managed types" — `bz_box_release` is the discharge.
//
// Shape coverage — every position each fix closes, plus the over-release
// oracles the fix MUST NOT break:
//   #3351:
//     (A) The issue's primary repro — `var ob: Object := h.M() as Object;`,
//         loop-amplified 20× so LSan flips r.exitCode false without the
//         fix; the value assertions pin correctness independently.
//     (B) OVER-RELEASE ORACLE — a bound Object local whose initializer
//         is a DIRECT call to `boxReturnMethods_` (no `as` cast). The
//         new arm MUST NOT fire here (no `as` cast in the AST), so the
//         existing #2865 case (b) handles the scope-end release exactly
//         once. This is the `isPlusOneBoxProducer` path the `as`
//         extension is layered on top of; we verify it is unchanged.
//   #3352:
//     (C) The issue's primary repro — `Console.WriteLine(e[k])` where
//         `E.property [i: int]: Object => i;`, loop-amplified 20×.
//     (D) §18.5 interpolation hole — `$"[{e[k]}]"` with the SAME indexer.
//     (E) Inline expression-bodied PROPERTY — `Console.WriteLine(p.O)`
//         where `property O: Object => _n;` on a value-typed backing.
//         The inline mint path is `lowerVirtualPropertyGet`'s
//         `boxForTarget` (~line 5505), the PROPERTY twin of #3352's
//         indexer inline path.
//     (F) OVER-RELEASE ORACLE — a method returning a STORED FIELD (the
//         pre-pass `collectBoxReturnMethods` rejects it; without the gate
//         the new arm would `emitBoxRelease` on a non-mint). The
//         #2876 test (G) covers this; we re-state it here so the test
//         file stands alone.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 6 checked rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics, and then RETURNED from the row on
// it. 2 of them ran their assertion block only inside an
// `if (!wasSkipped(...))` — the same SKIP test in POSITIVE polarity — so
// those rows skipped their assertions outright as well.
// That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a silent
// vacuous pass: on a box whose clang cannot link the ASan binary, all
// 6 rows returned WITHOUT asserting and the suite reported a clean PASS
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

// ===========================================================================
// #3351 (A) — `var ob: Object := h.M() as Object;` — the issue's primary
// repro. `M(): Object => _n;` mints a fresh box at its return edge; the
// `as Object` cast returns the operand (the box), per §17.4. The local
// owns the +1 and discharges at scope end — once `boxLocalAssignmentSourceKind`
// recognizes this shape as a +1 producer.
// ===========================================================================
const char* k3351_ObjectAsObject =
    "module M3351A; "
    "public class H3351A "
    "  private _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public method M(): Object => _n; "
    "end H3351A; "
    "public class C3351A "
    "  public static method Main(): int "
    "    using var h := new H3351A(5); "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      var ob: Object := h.M() as Object; "
    "      if ob is int then t := t + 1; end if; "
    "      k := k + 1; "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C3351A;";

// ===========================================================================
// #3351 (B) — OVER-RELEASE ORACLE — a bound Object local whose initializer
// is a DIRECT call to `boxReturnMethods_` (no `as` cast). The new arm MUST
// NOT fire here (no `as` cast in the AST), so the existing #2865 case (b)
// handles the scope-end release exactly once. This is the `isPlusOneBoxProducer`
// path the `as` extension is layered on top of; we verify it is unchanged.
//
// (The interface-target generalization — `as I3351B` — would have to
// SUCCEED at runtime for the same +1 forwarding to fire, which requires a
// non-empty interface that the boxed value actually implements; an int box
// does not implement an empty `interface I end I`, so `as I` returns null
// and the box is lost on a DIFFERENT code path. That is a #2865 sibling
// not closed here; the predicate gate is the same as Object, so the
// predicate correctness is covered by cell (A) above, which exercises the
// identical `as Object` shape and is the issue's own repro.)
// ===========================================================================
const char* k3351_BoundNoAs =
    "module M3351B; "
    "public class H3351B "
    "  private _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public method M(): Object => _n; "
    "end H3351B; "
    "public class C3351B "
    "  public static method Main(): int "
    "    using var h := new H3351B(5); "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      var ob: Object := h.M(); "
    "      if ob is int then t := t + 1; end if; "
    "      k := k + 1; "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C3351B;";

// ===========================================================================
// #3352 (D) — `Console.WriteLine(e[k])` where
// `E.property [i: int]: Object => i;`. The indexer's expression body
// lowers to `int i` (the parameter load), and `lowerIndexerGet`'s
// `boxForTarget(result, Object, getterExpr)` at ~line 2141 mints a fresh
// box for the `Object`-typed indexer; `lastBoxMint_` records the mint.
// `lowerExpr(e[k])` returns the box pointer (which IS `lastBoxMint_`), and
// `lowerObjectToString` dispatches the box's vtable ToString — mints a
// fresh string (released by the `releaseRender` arm) and does NOT retain
// the box. The new `inlineBoxMint` arm in `tryLowerConsoleCall` discharges
// the box. Without it the box leaks 24 B/eval (480 B / 20 allocations
// measured on the issue's repro).
// ===========================================================================
const char* k3352_IndexerCallArg =
    "module M3352D; "
    "public class E3352D "
    "  public property [i: int]: Object => i; "
    "end E3352D; "
    "public class C3352D "
    "  public static method Main(): int "
    "    var e := new E3352D(); "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      Console.WriteLine(e[k]); "
    "      k := k + 1; "
    "      t := t + 1; "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C3352D;";

// ===========================================================================
// #3352 (E) — §18.5 interpolation hole — `$"[{e[5]}]"` with the SAME
// indexer (the indexer's body returns the index; `e[5]` is always the
// single-digit value 5, so `s = "[5]"` is always length 3, and the loop
// can run 20× without double-digit interference). `lowerExpr(e[5])` mints
// the box inline; the new `inlineBoxMint` arm in `appendPart` discharges
// it. The §18.5 sibling consumption position, closed in lockstep with
// the call-arg position — same predicate (`arg == lastBoxMint_`), same
// discharge (`emitBoxRelease(srcEv)`).
// ===========================================================================
const char* k3352_IndexerInterp =
    "module M3352E; "
    "public class E3352E "
    "  public property [i: int]: Object => i; "
    "end E3352E; "
    "public class C3352E "
    "  public static method Main(): int "
    "    var e := new E3352E(); "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      var s: string := $\"[{e[5]}]\"; "
    "      k := k + 1; "
    "      t := t + (if s.Length = 3 then 1 else 0); "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C3352E;";

// ===========================================================================
// #3352 (F) — Inline expression-bodied PROPERTY — `Console.WriteLine(p.O)`
// where `property O: Object => _n;`. The property's inline body
// (`lowerVirtualPropertyGet` line ~5505) is the PROPERTY twin of the
// indexer's inline path — `boxForTarget(v, ctx_.exprType(*ma), getter)`
// mints a fresh box. Verifies the fix covers both inline-body surfaces,
// not just the indexer.
// ===========================================================================
const char* k3352_PropertyCallArg =
    "module M3352F; "
    "public class P3352F "
    "  private _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public property O: Object => _n; "
    "end P3352F; "
    "public class C3352F "
    "  public static method Main(): int "
    "    using var p := new P3352F(5); "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      Console.WriteLine(p.O); "
    "      k := k + 1; "
    "      t := t + 1; "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C3352F;";

// ===========================================================================
// #3352 (G) — OVER-RELEASE ORACLE — a method returning a STORED FIELD is
// a borrow, NOT a +1 box producer (the pre-pass `collectBoxReturnMethods`
// rejects it). The new arm MUST NOT fire here. Without the gate (no
// inline mint happens at the `M()` call site — `lastBoxMint_` stays null),
// the arm correctly declines; the bound local's scope-end release handles
// the borrow.
// ===========================================================================
const char* k3352_StoredFieldNoOverRelease =
    "module M3352G; "
    "public class H3352G "
    "  public S: string := \"self\"; "
    "  public method M(): Object => S; "
    "end H3352G; "
    "public class C3352G "
    "  public static method Main(): int "
    "    using var h := new H3352G(); "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      Console.WriteLine(h.M()); "
    "      k := k + 1; "
    "      t := t + 1; "
    "    end while; "
    "    if h.S = \"self\" then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C3352G;";

}  // namespace

// ===========================================================================
// #3351 (A) — the issue's primary repro.
// ===========================================================================
BEZEL_TEST(codegen_box_mint_residuals_3351_3352,
           cast_as_object_reclaims_box_spec_17_4) {
    JitResult plain = runAot(k3351_ObjectAsObject);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult r = runAotChecked(k3351_ObjectAsObject);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // pre-fix: 1 (LSan reports 480 B / 20)
}

// ===========================================================================
// #3351 (B) — OVER-RELEASE ORACLE — direct box-return call (no `as`).
// ===========================================================================
BEZEL_TEST(codegen_box_mint_residuals_3351_3352,
           direct_box_return_local_no_double_release_spec_28_3) {
    JitResult plain = runAot(k3351_BoundNoAs);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult lenient = runAotCheckedLenient(k3351_BoundNoAs);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(lenient));
    BEZEL_EXPECT_TRUE(lenient.ran);
    BEZEL_EXPECT_EQ(lenient.exitCode, 0);
    JitResult r = runAotChecked(k3351_BoundNoAs);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// #3352 (D) — `Console.WriteLine(e[k])` with inline indexer body.
// ===========================================================================
BEZEL_TEST(codegen_box_mint_residuals_3351_3352,
           indexer_inline_box_in_call_arg_reclaimed_spec_23_4) {
    JitResult plain = runAot(k3352_IndexerCallArg);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult r = runAotChecked(k3352_IndexerCallArg);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // pre-fix: 1 (LSan reports 480 B / 20)
}

// ===========================================================================
// #3352 (E) — `$"[{e[k]}]"` §18.5 interpolation-hole sibling.
// ===========================================================================
BEZEL_TEST(codegen_box_mint_residuals_3351_3352,
           indexer_inline_box_in_interp_hole_reclaimed_spec_18_5) {
    JitResult plain = runAot(k3352_IndexerInterp);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult r = runAotChecked(k3352_IndexerInterp);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// #3352 (F) — Inline property body in call-arg.
// ===========================================================================
BEZEL_TEST(codegen_box_mint_residuals_3351_3352,
           property_inline_box_in_call_arg_reclaimed_spec_23_2) {
    JitResult plain = runAot(k3352_PropertyCallArg);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult r = runAotChecked(k3352_PropertyCallArg);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// #3352 (G) — OVER-RELEASE ORACLE — stored-field return is a borrow, not
// a +1 producer.
// ===========================================================================
BEZEL_TEST(codegen_box_mint_residuals_3351_3352,
           stored_field_return_in_call_arg_no_double_release_spec_28_3) {
    JitResult plain = runAot(k3352_StoredFieldNoOverRelease);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult lenient = runAotCheckedLenient(k3352_StoredFieldNoOverRelease);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(lenient));
    BEZEL_EXPECT_TRUE(lenient.ran);
    BEZEL_EXPECT_EQ(lenient.exitCode, 0);
    JitResult r = runAotChecked(k3352_StoredFieldNoOverRelease);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
