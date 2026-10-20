// Codegen — a §8.6 BOX was never reclaimed in three shapes: an
// `Object`-returning METHOD used as a bare temporary, an `Object`-typed INDEXER
// read used as a bare temporary, and INTERPOLATING an `Object` local that holds
// a box. Issue #2865.
//
// Spec (revision v0.240):
//   §8.6 "Boxing" — "When a value type is used where `Object` (or an interface
//     implemented by the value type) is expected, the value is **boxed**: a
//     heap-allocated wrapper holds the value. Boxed values are runtime-managed
//     ([§28.9])."
//   §28.9 "Runtime-managed types" — a boxed value type is "exempt from manual
//     disposal", and "The implementation of runtime-managed types' lifecycle
//     (reference counting, region allocation, or other) is not part of this
//     specification". That leaves the MECHANISM open and the OBLIGATION intact:
//     exempting the USER from writing a disposal is not exempting codegen from
//     performing one.
//   §28.3 "Local variables, ownership, and borrows" — the category the two
//     temporary shapes fall in: a value with no binding and no owner.
//   §18.5 "Interpolated strings" — the third shape's construct.
//   §17.4 "The `is`, `is not`, and `as` operators" — the two consumption
//     positions the fix reclaims at.
//
// MEASURED on bd209a2 with `bezelc --sanitize=address`, 20 iterations each:
// every one of the three shapes leaked 480 bytes in 20 objects (24 B per
// evaluation, unbounded in a loop). The neighbourhood was measured too, and it
// is what shaped the fix:
//     h.M() is int          LEAK   (shape 1 — callee-minted box, no binding)
//     e[i]  is int          LEAK   (shape 2 — inline mint, no binding)
//     h.O   is int          LEAK   (same family: a property read temporary)
//     h.M() as int          LEAK
//     var o := h.M(); o is int      CLEAN — the bound-local release already
//                                   existed (isPlusOneBoxProducer case (b))
//     var op: Object := …; $"{op}"  LEAK   (shape 3)
//     var op: Object := …; op.ToString()   CLEAN — the SAME local, the same
//                                   box, the same rendering: the only
//                                   difference is BoxEscapeWalker's verdict on
//                                   the two positions.
//     var o: Object := k; Console.WriteLine(o)   LEAK (same walker gap)
//
// THE FIX, in two independent halves:
//   (1) BoxEscapeWalker now treats an INTERPOLATION HOLE and a
//       `Console.Write`/`WriteLine` VALUE ARGUMENT as non-consuming reads —
//       exactly the two relaxations StringEscapeWalker already carries (its own
//       interpolation arm, and #1994 for Console). Neither position retains the
//       box, so the local stays eligible for the scope-end `bz_box_release`
//       that already existed.
//   (2) `lowerTypeTest` reclaims a PROVABLY FRESH box operand at `is` /
//       `is not` / `as <value type>` — the positions that walk the descriptor
//       or copy the payload bytes and keep nothing. Freshness has two proofs: a
//       mint-identity check against `boxValue`'s last mint (covers a box minted
//       in this frame while lowering the operand), or `isUserMethodBoxReturnCall`
//       (covers a box minted by a `boxReturnMethods_` callee and transferred).
//
// ⚠ ORACLE DISCIPLINE. Every leak case is LOOP-AMPLIFIED (20 iterations plus a
// runtime-built churn string) — a one-shot `Main` is not a reliable LSan
// oracle, and the issue itself recorded a 480/20-vs-456/19 discrepancy caused
// by nothing but the last iteration still being reachable at exit. Every string
// is built at RUNTIME (`"c" + i.ToString()`), never folded from two literals,
// which would make the probe vacuous on an immortal rc sentinel. And the
// OVER-release direction — which "no leak" is blind to — has its own oracles
// below: programs that re-read the same box AFTER every position the fix now
// releases or borrows at, run under generated-code ASan with leak detection
// OFF, so a premature free surfaces as a heap-use-after-free rather than as a
// missing leak.
//
// ⚠ KNOWN RESIDUALS, deliberately still leaking and filed rather than guessed
// at: an unbound box temporary in a CALL-ARGUMENT position
// (`Console.WriteLine(h.M())`) or in an INTERPOLATION hole (`$"{h.M()}"`), and
// `var o: Object := h.M() as Object;`. Each needs an owner for a value the
// consumer never binds; the safe direction is the bounded leak, not a guess.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// #3733 toolchain-SKIP burn-down. This file's ten single-result rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics and returned from the row on it,
// and `bound_box_local_released_exactly_once_spec_28_9` used the same
// predicate in its positive-polarity form to skip the LENIENT leg's
// assertions outright. That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a
// silent vacuous pass: on a box whose clang cannot link the ASan binary,
// all ten rows returned WITHOUT asserting and the suite reported a clean
// PASS having compiled and run nothing. MEASURED on the pre-change tree
// with the harness's own knob (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`):
// 10 passed, 0 failed, and `CHECKED TOOLCHAIN: 11 skip(s)`.
//
// Every one of those guards becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position:
// the skip is now LOUD — the row FAILS — instead of being absorbed by a
// guard that reports a clean pass. The lenient leg's two
// assertions additionally now RUN UNCONDITIONALLY rather than being
// skipped, which is the other half of the strengthening. Every
// substantive assertion in every row (r.ran and the exitCode pins) stays
// exactly where it was.
//
// The `runAot` leg each row opens with is untouched and is not a checked
// call, so it is not part of the absorbing surface; it is the plain
// (unsanitized) leg these rows have always run alongside the ASan/LSan
// one.
// --- Shape 1: an `Object`-returning METHOD used as a bare temporary. --------
const char* kShape1MethodTemp =
    "module M2865A; "
    "public class Mth2865A "
    "  public method M(): Object => 6; "
    "end Mth2865A; "
    "public class C2865A "
    "  public static method Main(): int "
    "    using var m := new Mth2865A(); "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      if m.M() is int then t := t + 1; end; "
    "      var churn: string := \"c\" + k.ToString(); "
    "      t := t + churn.Length - churn.Length; "
    "      k := k + 1; "
    "    end while; "
    "    if t = 20 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end C2865A;";

// --- Shape 2: an `Object`-typed expression-bodied INDEXER read. -------------
const char* kShape2IndexerTemp =
    "module M2865B; "
    "public class E2865B "
    "  public property [i: int]: Object => i; "
    "end E2865B; "
    "public class C2865B "
    "  public static method Main(): int "
    "    using var e := new E2865B(); "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      if e[k] is int then t := t + 1; end; "
    "      var churn: string := \"c\" + k.ToString(); "
    "      t := t + churn.Length - churn.Length; "
    "      k := k + 1; "
    "    end while; "
    "    if t = 20 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end C2865B;";

// --- Shape 3: INTERPOLATING an `Object` local that holds a box. -------------
// `op` is declared INSIDE the loop so all 20 boxes are unreachable at exit —
// the issue's own 456/19 measurement came from the last iteration's box still
// being live, which is exactly the one-shot-oracle hazard this shape warns
// about. The rendered length is asserted so a "fix" that stopped rendering
// would not pass.
const char* kShape3InterpolatedBoxLocal =
    "module M2865C; "
    "public record Pt2865C(X: int, Y: int); "
    "public class C2865C "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      var op: Object := new Pt2865C(k, k); "
    "      var s1: string := $\"[{op}]\"; "
    "      var churn: string := \"c\" + k.ToString(); "
    "      t := t + s1.Length + churn.Length - churn.Length; "
    "      k := k + 1; "
    "    end while; "
    "    if t = 440 then return 41; end; "   // sum of the rendered lengths
    "    return 1; "
    "  end Main; "
    "end C2865C;";

// --- The `Console.WriteLine` sibling of shape 3 (same walker gap). ----------
const char* kConsoleWriteBoxLocal =
    "module M2865D; "
    "public class C2865D "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      var o: Object := k; "
    "      Console.WriteLine(o); "
    "      var churn: string := \"c\" + k.ToString(); "
    "      t := t + churn.Length - churn.Length + 1; "
    "      k := k + 1; "
    "    end while; "
    "    if t = 20 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end C2865D;";

// --- The boxable-KIND grid over both consumption positions. ----------------
// `decimal` boxes only since #2793 and an IMPORTED record only since #2796; the
// LOCAL record/enum, the primitives and a CLASS-instance control are all here
// so the release is pinned across the whole family rather than for `int`.
// (The imported half is a separate module test — see the header note.)
const char* kKindGridTemps =
    "module M2865E; "
    "public record Pt2865E(X: int, Y: int); "
    "public enum Color2865E Red, Green; end Color2865E; "
    "public class K2865E "
    "  public v: int; "
    "  public constructor(n: int) v := n; end constructor; "
    "end K2865E; "
    "public class H2865E "
    "  private _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public method Mi(): Object => _n; "
    "  public method Md(): Object => 2.5; "
    "  public method Mc(): Object => 'z'; "
    "  public method Mb(): Object => true; "
    "  public method Mm(): Object => 12.5m; "
    "  public method Mr(): Object => new Pt2865E(_n, _n); "
    "  public method Me(): Object => Color2865E.Green; "
    "  public property [i: int]: Object => _n + i; "
    "end H2865E; "
    "public class C2865E "
    "  public static method Main(): int "
    "    using var h := new H2865E(7); "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      if h.Mi() is int        then t := t + 1; end; "
    "      if h.Md() is double     then t := t + 1; end; "
    "      if h.Mc() is char       then t := t + 1; end; "
    "      if h.Mb() is bool       then t := t + 1; end; "
    "      if h.Mm() is decimal    then t := t + 1; end; "
    "      if h.Mr() is Pt2865E    then t := t + 1; end; "
    "      if h.Me() is Color2865E then t := t + 1; end; "
    "      if h[k] is int          then t := t + 1; end; "
    "      var q: int? := h.Mi() as int; "
    "      t := t + (q ?? 0); "
    "      using var kk := new K2865E(k); "        // class control: never boxed
    "      var oc: Object := kk; "
    "      if oc is K2865E then t := t + 1; end; "
    "      var churn: string := \"c\" + k.ToString(); "
    "      t := t + churn.Length - churn.Length; "
    "      k := k + 1; "
    "    end while; "
    "    if t = 320 then return 41; end; "        // 20 * (9 + 7)
    "    return 1; "
    "  end Main; "
    "end C2865E;";

// --- OVER-RELEASE ORACLE 1: the same box is read AFTER every borrowing -----
// position the walker relaxation now admits, and after the `is`/`as` the fix
// releases at. A premature free is a heap-use-after-free on the next read.
// Run with LEAKS OFF so the assertion is purely about the UAF direction.
const char* kOverReleaseBorrowPositions =
    "module M2865F; "
    "public record Pt2865F(X: int, Y: int); "
    "public class C2865F "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      var op: Object := new Pt2865F(k, k + 1); "
    "      var s1: string := $\"[{op}]\"; "
    "      Console.WriteLine(op); "
    "      var s2: string := $\"[{op}]\"; "         // second render of the SAME box
    "      if op is Pt2865F then t := t + 1; end; "
    "      var r: Pt2865F? := op as Pt2865F; "     // unbox AFTER the renders
    "      if r <> null then t := t + (r!).Y; end; "
    "      t := t + s1.Length - s2.Length; "
    "      k := k + 1; "
    "    end while; "
    "    if t = 230 then return 41; end; "         // 20 + sum(k+1, k=0..19)
    "    return 1; "
    "  end Main; "
    "end C2865F;";

// --- OVER-RELEASE ORACLE 2: a box BOUND to a local must be released exactly
// ONCE — by the pre-existing scope-end disposal, never additionally by the new
// `is`/`as` temporary release. Two `is` tests and an `as` on the SAME local, so
// a wrongly-firing temp release would be a double free (ASan aborts) and a
// dangling read.
const char* kBoundLocalNotDoubleReleased =
    "module M2865G; "
    "public class H2865G "
    "  private _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public method M(): Object => _n; "
    "end H2865G; "
    "public class C2865G "
    "  public static method Main(): int "
    "    using var h := new H2865G(7); "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      var o := h.M(); "
    "      if o is int then t := t + 1; end; "
    "      if o is int then t := t + 1; end; "
    "      var q: int? := o as int; "
    "      t := t + (q ?? 0); "
    "      if o is int then t := t + 1; end; "     // read again after the unbox
    "      k := k + 1; "
    "    end while; "
    "    if t = 200 then return 41; end; "         // 20 * (3 + 7)
    "    return 1; "
    "  end Main; "
    "end C2865G;";

// --- OVER-RELEASE ORACLE 3: `as` to a REFERENCE target RETURNS the operand,
// so the fix must NOT release there. A boxed record cast to `Object` and to an
// INTERFACE it implements, then dereferenced through the result.
const char* kAsReferenceTargetKeepsTheBox =
    "module M2865H; "
    "public interface IArea2865H method Area(): int; end IArea2865H; "
    "public record Sq2865H(s: int) : IArea2865H "
    "  public method Area(): int => s * s; "
    "end Sq2865H; "
    "public class H2865H "
    "  public method R(): Object => new Sq2865H(3); "
    "end H2865H; "
    "public class C2865H "
    "  public static method Main(): int "
    "    using var h := new H2865H(); "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      var ob: Object := h.R() as Object; "
    "      if ob is Sq2865H then t := t + 1; end; "
    "      var ia: IArea2865H? := h.R() as IArea2865H; "
    "      if ia <> null then t := t + (ia!).Area(); end; "
    "      k := k + 1; "
    "    end while; "
    "    if t = 200 then return 41; end; "         // 20 * (1 + 9)
    "    return 1; "
    "  end Main; "
    "end C2865H;";

// --- OVER-CORRECTION GUARD: a CLASS instance through `Object` is never boxed
// (§8.6 boxes a VALUE type), so neither the walker relaxation nor the temp
// release may touch it. Its own disposal must still run exactly once — the
// program interpolates it, type-tests it, and then reads the instance again.
//
// ⚠ `Console.WriteLine(oc)` is deliberately ABSENT here. That construct —
// `Console.Write`/`WriteLine` on an `Object`-typed local holding a REFERENCE —
// aborts with `free(): invalid pointer` for a class and corrupts the source
// string for a `string`, on the PARENT commit as well as with this fix. It is a
// pre-existing over-release with nothing to do with either half of #2865 (the
// box walker never tracks a reference-initialized local), it is filed
// separately, and including it here would make this control fail for a reason
// this test is not about. The BOX spelling of the same call IS exercised, by
// kConsoleWriteBoxLocal and kOverReleaseBorrowPositions above.
const char* kClassThroughObjectUntouched =
    "module M2865I; "
    "public class K2865I "
    "  public v: int; "
    "  public constructor(n: int) v := n; end constructor; "
    "end K2865I; "
    "public class C2865I "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      using var kk := new K2865I(k); "
    "      var oc: Object := kk; "
    "      var s: string := $\"[{oc}]\"; "
    "      if oc is K2865I then t := t + 1; end; "
    "      t := t + kk.v - kk.v + s.Length - s.Length; "
    "      k := k + 1; "
    "    end while; "
    "    if t = 20 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end C2865I;";

// --- OVER-CORRECTION GUARD: a STRING through `Object` is never boxed either
// (§28.9 — a string is runtime-managed in its own right and stays a bare
// `bz_string*` in an Object slot). The walker relaxation must not arm a
// `bz_box_release` on it, and the source string must survive being re-read
// after the interpolation. (Same pre-existing-abort caveat as the guard above:
// no `Console.WriteLine(os)` here.)
const char* kStringThroughObjectUntouched =
    "module M2865J; "
    "public class C2865J "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      var src: string := \"v\" + k.ToString(); "
    "      var os: Object := src; "
    "      var s: string := $\"[{os}]\"; "
    "      if os is string then t := t + 1; end; "
    "      t := t + src.Length - src.Length + s.Length - s.Length; "
    "      k := k + 1; "
    "    end while; "
    "    if t = 20 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end C2865J;";

}  // namespace

// ===========================================================================
// (A) The three shapes the issue names. LSan forces exit 1 on any leak, so
// `exitCode == 41` under runAotChecked IS the leak assertion.
// ===========================================================================
// per §8.6, §28.9, §28.3 — shape 1.
BEZEL_TEST(codegen_box_temp_release_and_interp_2865,
           object_returning_method_temp_is_reclaimed_spec_28_9) {
    JitResult plain = runAot(kShape1MethodTemp);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotChecked(kShape1MethodTemp);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);   // pre-fix: 1 (480 B in 20 objects)
}

// per §8.6, §23.4, §28.3 — shape 2.
BEZEL_TEST(codegen_box_temp_release_and_interp_2865,
           object_indexer_read_temp_is_reclaimed_spec_28_9) {
    JitResult plain = runAot(kShape2IndexerTemp);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotChecked(kShape2IndexerTemp);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);   // pre-fix: 1 (480 B in 20 objects)
}

// per §18.5, §8.6, §28.9 — shape 3.
BEZEL_TEST(codegen_box_temp_release_and_interp_2865,
           interpolated_box_local_is_reclaimed_spec_18_5) {
    JitResult plain = runAot(kShape3InterpolatedBoxLocal);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotChecked(kShape3InterpolatedBoxLocal);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);   // pre-fix: 1 (480 B in 20 objects)
}

// per §8.6, §28.9 — the Console sibling of shape 3 (same walker gap, measured
// leaking at the same 480/20).
BEZEL_TEST(codegen_box_temp_release_and_interp_2865,
           console_written_box_local_is_reclaimed_spec_28_9) {
    JitResult plain = runAot(kConsoleWriteBoxLocal);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotChecked(kConsoleWriteBoxLocal);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);   // pre-fix: 1
}

// per §8.6 — the whole boxable-kind family, both positions.
BEZEL_TEST(codegen_box_temp_release_and_interp_2865,
           box_temp_kind_grid_is_reclaimed_spec_8_6) {
    JitResult plain = runAot(kKindGridTemps);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotChecked(kKindGridTemps);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);   // pre-fix: 1
}

// ===========================================================================
// (B) The OVER-RELEASE direction, which a "no leak" assertion cannot see.
// `runAotCheckedLenient` = generated-code ASan with leak detection OFF, so a
// premature free surfaces as a heap-use-after-free abort, never as a leak.
// ===========================================================================
// per §18.5, §8.6 — the box is re-read after every borrowing position.
BEZEL_TEST(codegen_box_temp_release_and_interp_2865,
           borrow_positions_do_not_free_the_box_spec_28_3) {
    JitResult plain = runAot(kOverReleaseBorrowPositions);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotCheckedLenient(kOverReleaseBorrowPositions);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// per §28.3, §28.9 — EXACTLY-ONCE: a bound local's box is released by its
// scope-end disposal only, never additionally by the new temporary release.
BEZEL_TEST(codegen_box_temp_release_and_interp_2865,
           bound_box_local_released_exactly_once_spec_28_9) {
    JitResult lenient = runAotCheckedLenient(kBoundLocalNotDoubleReleased);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(lenient));
    BEZEL_EXPECT_TRUE(lenient.ran);
    BEZEL_EXPECT_EQ(lenient.exitCode, 41);   // no double free / no UAF
    JitResult r = runAotChecked(kBoundLocalNotDoubleReleased);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);             // and no leak either
}

// per §17.4 — `as` to a REFERENCE target yields the operand itself, so the box
// must survive the cast and stay dereferenceable.
BEZEL_TEST(codegen_box_temp_release_and_interp_2865,
           as_reference_target_keeps_the_box_spec_17_4) {
    JitResult plain = runAot(kAsReferenceTargetKeepsTheBox);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotCheckedLenient(kAsReferenceTargetKeepsTheBox);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// ===========================================================================
// (C) Over-correction guards — green BEFORE and AFTER the fix.
// ===========================================================================
// per §8.6 ("a VALUE type used where Object is expected") — a class reference
// is not boxed, so neither half of this fix may touch its lifetime.
BEZEL_TEST(codegen_box_temp_release_and_interp_2865,
           class_through_object_untouched_spec_8_6) {
    JitResult plain = runAot(kClassThroughObjectUntouched);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotChecked(kClassThroughObjectUntouched);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// per §28.9 — a string in an Object slot is a bare `bz_string*`, never a box.
//
// ⚠ The LENIENT oracle (ASan on, leak detection OFF) is the right one here, and
// the reason is measured, not defensive: `var os: Object := src;` binds a
// STRING local into an Object local, which StringEscapeWalker treats as an
// escape, so `src`'s +1 is deliberately left un-released — the ~36 B/iteration
// residual #2846 documented and explicitly declined to change (that walker's
// safe direction). It reproduces identically on the parent commit (710 bytes in
// 40 allocations over 20 iterations) and neither half of #2865 touches it. What
// this guard is actually for is the OVER-release direction: `src` is re-read
// after the interpolation, so a `bz_box_release` wrongly armed on a
// string-initialized Object local would be a heap-use-after-free here.
BEZEL_TEST(codegen_box_temp_release_and_interp_2865,
           string_through_object_untouched_spec_28_9) {
    JitResult plain = runAot(kStringThroughObjectUntouched);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotCheckedLenient(kStringThroughObjectUntouched);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}
