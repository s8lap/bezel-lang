// Codegen — an Object-typed METHOD CALL whose body provably always returns a
// fresh §8.6 box is itself a +1 box producer (#2865's pre-pass
// `collectBoxReturnMethods`). Consumed at `Console.Write`/`WriteLine` (per
// stdlib §2.1, `value: Object`) or at a §18.5 interpolation hole, the
// callee's +1 box is orphaned unless an extra discharge fires at the call
// site. Without that discharge the box leaks 24 B/call, unbounded in a loop
// (480 B / 20 allocations over 20 iterations, the measured shape on
// `f693622`).
//
// This is the BOX twin of #2879 (which closed the same shape for +1 string
// producers). #2865 closed the inline / `lastBoxMint_` shape (an unbound
// `is`/`is not`/`as <value type>` box temp) and the bound-local shape
// (an Object-typed local whose initializer is a CallExpr in
// `boxReturnMethods_`, released at scope end by `collectBoxReleaseEligible`).
// #2865 did NOT close the unbound box temp at the call-argument and
// interpolation-hole positions, where the consumer does not bind the box to
// a local — that is the residual #2876 filed.
//
// Spec (revision v0.278):
//   §8.6 "Boxing" — value types in Object slots box; the wrapper is
//     runtime-managed (§28.9) but the +1 ownership contract is the same as
//     for every other heap object.
//   §18.5 "Interpolated strings" — each hole renders by §8.5.
//   §28.3 "Local variables, ownership, and borrows" — an un-bound temporary
//     has no owner; the §28.3 unbound-owned-temporary rule disposes at the
//     end of the full expression. A call-argument and an interpolation hole
//     both qualify.
//   §28.9 "Runtime-managed types" — the +1 ownership contract for boxes.
//
// Shape coverage — every consumption position the fix closes, plus the
// over-release oracle shapes the fix MUST NOT break:
//   (A) Console.WriteLine(h.M())             — the issue's primary shape
//   (B) Console.Write(h.M())                 — sibling without trailing \n
//   (C) $"[{h.M()}]"                         — §18.5 interpolation hole
//   (D) Block-bodied method body that always returns a boxable value
//   (E) Property [i: int]: Object => i;      — same pre-pass class as #2865 shape 2
//   (F) OVER-RELEASE ORACLE — bound Object local (the #2876 predicate is an
//       IdentifierExpr read, NOT a CallExpr; the local's scope-end release
//       handles its borrow; the new arm must NOT fire here, else it would
//       over-release)
//   (G) OVER-RELEASE ORACLE — a method returning a STORED FIELD is a borrow
//       (the field is borrowed, not produced); the pre-pass must NOT admit
//       it; without the gate the extra discharge would over-release the
//       bound `_n` storage
//
// ⚠ ORACLE DISCIPLINE (mirrors object_temp_string_leak_2879_test.cpp and
// box_temp_release_and_interp_2865_test.cpp): every leak case is
// LOOP-AMPLIFIED (20 iterations) so a one-shot `Main` is not a reliable LSan
// oracle. The over-release direction has its own oracle: a stored field is
// re-read after every Console.Write that touches it (a premature free would
// be a heap-use-after-free), which `runAotCheckedLenient` catches as a
// heap-use-after-free under generated-code ASan with leak detection off.

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
// (A) The issue's primary repro — `Console.WriteLine(h.M())` where
// `M(): Object => _n;` always returns a boxable value (an int).
// ===========================================================================
const char* kConsoleWriteLineProducer =
    "module M2876A; "
    "public class H2876A "
    "  private _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public method M(): Object => _n; "
    "end H2876A; "
    "public class C2876A "
    "  public static method Main(): int "
    "    using var h := new H2876A(5); "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      Console.WriteLine(h.M()); "
    "      k := k + 1; "
    "      t := t + 1; "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C2876A;";

// ===========================================================================
// (B) Console.Write (no trailing \n) — same path, narrower write.
// ===========================================================================
const char* kConsoleWriteProducer =
    "module M2876B; "
    "public class H2876B "
    "  private _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public method M(): Object => _n; "
    "end H2876B; "
    "public class C2876B "
    "  public static method Main(): int "
    "    using var h := new H2876B(5); "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      Console.Write(h.M()); "
    "      k := k + 1; "
    "      t := t + 1; "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C2876B;";

// ===========================================================================
// (C) §18.5 interpolation hole — the same defect, different consumption.
// ===========================================================================
const char* kInterpolationProducer =
    "module M2876C; "
    "public class H2876C "
    "  private _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public method M(): Object => _n; "
    "end H2876C; "
    "public class C2876C "
    "  public static method Main(): int "
    "    using var h := new H2876C(5); "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      var s: string := $\"[{h.M()}]\"; "
    "      k := k + 1; "
    "      t := t + (if s.Length = 3 then 1 else 0); "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C2876C;";

// ===========================================================================
// (D) Block-bodied method body that always returns a boxable value (every
// ReturnStmt in the body passes `isBoxableValueType` / `isNullableValueTy`).
// The Scanner walks the body and ANDs "every return is a boxable" + ORs
// "any return exists".
// ===========================================================================
const char* kBlockBodyProducer =
    "module M2876D; "
    "public class H2876D "
    "  private _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public method M(): Object "
    "    if _n > 0 then return _n; end; "
    "    return _n + 1; "
    "  end M; "
    "end H2876D; "
    "public class C2876D "
    "  public static method Main(): int "
    "    using var h := new H2876D(5); "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      Console.WriteLine(h.M()); "
    "      k := k + 1; "
    "      t := t + 1; "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C2876D;";

// ===========================================================================
// (F) OVER-RELEASE ORACLE — bound Object local. The local's scope-end
// release handles its borrow; the new arm must NOT fire on the
// IdentifierExpr read inside Console.WriteLine. Without the predicate gate
// every Console.WriteLine would over-release.
// ===========================================================================
const char* kBoundObjectNoOverRelease =
    "module M2876F; "
    "public class C2876F "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      var n: int := k + 1; "
    "      var ob: Object := n; "
    "      Console.WriteLine(ob); "
    "      k := k + 1; "
    "      t := t + 1; "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C2876F;";

// ===========================================================================
// (G) OVER-RELEASE ORACLE — a method returning a STORED STRING FIELD is a
// borrow, NOT a +1 producer. The pre-pass must NOT admit it
// (`isBoxableValueType(storedStringField)` is false — a bare MemberAccessExpr
// read of a `string`-typed field is a string borrow, not a boxable value).
// Without the pre-pass gate the new arm would `emitBoxRelease` on a string
// pointer and corrupt the bz_string header on every Console.WriteLine, which
// `runAotCheckedLenient` catches as a heap-use-after-free.
// ===========================================================================
const char* kStoredFieldReturnNoOverRelease =
    "module M2876G; "
    "public class H2876G "
    "  public S: string := \"self\"; "
    "  public method M(): Object => S; "
    "end H2876G; "
    "public class C2876G "
    "  public static method Main(): int "
    "    using var h := new H2876G(); "
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
    "end C2876G;";

}  // namespace

// ===========================================================================
// (A) The Console.WriteLine shape — pre-fix leaked 480 B / 20 allocations.
// runAotChecked forces generated-code ASan + LSan; LSan forces exit 1 on
// any leak, so exitCode == 0 IS the leak assertion.
// ===========================================================================
BEZEL_TEST(codegen_object_temp_box_call_arg_2876,
           console_writeline_producer_is_reclaimed_spec_28_9) {
    JitResult plain = runAot(kConsoleWriteLineProducer);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult r = runAotChecked(kConsoleWriteLineProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // pre-fix: 1 (LSan reports 480 B / 20)
}

// ===========================================================================
// (B) Console.Write (no trailing \n) — the same path minus the newline.
// ===========================================================================
BEZEL_TEST(codegen_object_temp_box_call_arg_2876,
           console_write_producer_is_reclaimed_spec_28_9) {
    JitResult plain = runAot(kConsoleWriteProducer);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult r = runAotChecked(kConsoleWriteProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // pre-fix: 1
}

// ===========================================================================
// (C) §18.5 interpolation hole — the §2876 sibling consumption position.
// ===========================================================================
BEZEL_TEST(codegen_object_temp_box_call_arg_2876,
           interpolation_hole_producer_is_reclaimed_spec_18_5) {
    JitResult plain = runAot(kInterpolationProducer);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult r = runAotChecked(kInterpolationProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // pre-fix: 1 (480 B / 20 — same shape)
}

// ===========================================================================
// (D) Block-bodied method that always returns a boxable value — proves the
// Scanner's "every ReturnStmt is a boxable" gate admits the body shape.
// ===========================================================================
BEZEL_TEST(codegen_object_temp_box_call_arg_2876,
           block_body_producer_is_reclaimed_spec_28_3) {
    JitResult plain = runAot(kBlockBodyProducer);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult r = runAotChecked(kBlockBodyProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (F) OVER-RELEASE ORACLE — a bound Object local pointing at a borrow must
// NOT be released by the new arm. `ob`'s scope-end release absorbs the
// render's +1; an extra discharge would UAF the underlying int box.
// ===========================================================================
BEZEL_TEST(codegen_object_temp_box_call_arg_2876,
           bound_object_local_does_not_over_release_spec_28_3) {
    JitResult plain = runAot(kBoundObjectNoOverRelease);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult lenient = runAotCheckedLenient(kBoundObjectNoOverRelease);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(lenient));
    BEZEL_EXPECT_TRUE(lenient.ran);
    BEZEL_EXPECT_EQ(lenient.exitCode, 0);
    JitResult r = runAotChecked(kBoundObjectNoOverRelease);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (G) OVER-RELEASE ORACLE — a method returning a STORED STRING FIELD is a
// borrow, NOT a +1 producer. The pre-pass must NOT admit it; without the
// gate the new arm would `emitBoxRelease` on a string pointer and corrupt
// the bz_string header on every Console.WriteLine.
// ===========================================================================
BEZEL_TEST(codegen_object_temp_box_call_arg_2876,
           stored_field_return_does_not_over_release_spec_28_3) {
    JitResult plain = runAot(kStoredFieldReturnNoOverRelease);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult lenient = runAotCheckedLenient(kStoredFieldReturnNoOverRelease);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(lenient));
    BEZEL_EXPECT_TRUE(lenient.ran);
    BEZEL_EXPECT_EQ(lenient.exitCode, 0);
    JitResult r = runAotChecked(kStoredFieldReturnNoOverRelease);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}