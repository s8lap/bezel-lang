// Codegen — interface-DISPATCH sibling of #2879. The direct-Object shape is
// closed by the #2879 pre-pass; the same defect at one indirection deeper
// (an Object temporary handed off through an interface MethodSymbol) leaks.
// An interface dispatch to a +1 string producer
// (`Fresh(): Object => "x" + S`, accessed through `var o: IFoo := h;
// o.Fresh()`) is provably a +1 transfer: every concrete implementer of `IFoo`
// that's locally compiled produces a fresh string. Without an extra discharge
// at the call site the callee's +1 is orphaned — 38 B/call, unbounded in a
// loop (760 B in 40 allocations over 20 iterations, the measured shape on
// `eabfa69`).
//
// Spec (revision v0.240):
//   §8.5 "Rendering values for `Console.Write`/`WriteLine` and string
//     interpolation" — class-typed values render via virtual ToString dispatch;
//     for a string receiver `lowerObjectToString` returns the retained receiver.
//   §8.6 "Boxing" — value types in Object slots box; the present shape is a
//     STRING, which is NOT a value type — bare bz_string* in an Object slot.
//   §18.5 "Interpolated strings" — each hole renders by §8.5.
//   §28.3 "Local variables, ownership, and borrows" — an un-bound temporary
//     has no owner; #2865 is its box residual.
//   §28.9 "Runtime-managed types" — the +1 ownership contract for strings.
//
// Shape coverage — every consumption position the fix closes, plus the
// over-release / no-over-release oracle shapes the fix MUST NOT break:
//   (A) Console.WriteLine through interface dispatch — the issue's primary
//   (B) Console.Write through interface dispatch — sibling without trailing \n
//   (C) §18.5 interpolation hole through interface dispatch — the §2879/§2865
//       twin consumption position, with one indirection deeper
//   (D) Debug.WriteLine through interface dispatch — debug sibling, special-
//       lowered path
//   (E) OVER-RELEASE ORACLE — an interface dispatch to a method returning a
//       STORED FIELD (a borrow) must NOT trigger the producer arm. The
//       field-return method is NOT in objectStringReturnMethods_ so the extra
//       discharge does NOT fire; the lenient runAotCheckedLenient oracle
//       catches a premature free as a heap-use-after-free.
//   (F) NO-ADMIT ORACLE — multi-implementor interface (two classes implement
//       IFresh, only one is a +1 producer) must NOT be admitted: the call
//       could dispatch to the non-producer override and over-release. The
//       gate "exactly one locally-compiled implementer" keeps the fix
//       conservative.
//   (G) CLASS-typed receiver — the §2879 fix already covers the
//       concrete-receiver case; this shape (no interface dispatch) is the
//       control test that proves the interface arm did not regress the
//       direct-Object sibling.
//
// ⚠ ORACLE DISCIPLINE (mirrors object_temp_string_leak_2879_test.cpp):
// every leak case is LOOP-AMPLIFIED (20 iterations plus a runtime-built churn
// string) so a one-shot `Main` is not a reliable LSan oracle; the strings are
// built at RUNTIME (`"x" + S` after `h.S := "s" + "elf".Substring(0, 3)`,
// never folded from two literals — which would make the probe vacuous on an
// immortal rc sentinel. The over-release direction has its own oracle: a stored
// field is re-read after every Console.Write that touches it (a premature free
// would be a heap-use-after-free).

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
// (A) Console.WriteLine through interface dispatch — the issue's primary.
// One locally-compiled implementor (H3349IfaceA) of IFresh3349A. The interface
// MethodSymbol `Fresh` is abstract; the concrete `H3349IfaceA.Fresh` is in
// objectStringReturnMethods_. The call `o.Fresh()` resolves through the
// interface, but at runtime always reaches the producer.
// ===========================================================================
const char* kInterfaceConsoleWriteLineProducer =
    "module M3349A; "
    "public interface IFresh3349A "
    "  method Fresh(): Object; "
    "end IFresh3349A; "
    "public class H3349IfaceA: IFresh3349A "
    "  public S: string := \"h\"; "
    "  public method Fresh(): Object => \"x\" + S; "
    "end H3349IfaceA; "
    "public class C3349A "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    using var h := new H3349IfaceA(); "
    "    h.S := \"s\" + \"elf\".Substring(0, 3); "
    "    while i < 20 do "
    "      i := i + 1; "
    "      var o: IFresh3349A := h; "
    "      Console.WriteLine(o.Fresh()); "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + churn.Length - churn.Length + 1; "
    "    end while; "
    "    Console.WriteLine(h.S); "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C3349A;";

// ===========================================================================
// (B) Console.Write (no trailing newline) through interface dispatch.
// ===========================================================================
const char* kInterfaceConsoleWriteProducer =
    "module M3349B; "
    "public interface IFresh3349B "
    "  method Fresh(): Object; "
    "end IFresh3349B; "
    "public class H3349IfaceB: IFresh3349B "
    "  public S: string := \"h\"; "
    "  public method Fresh(): Object => \"x\" + S; "
    "end H3349IfaceB; "
    "public class C3349B "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    using var h := new H3349IfaceB(); "
    "    h.S := \"s\" + \"elf\".Substring(0, 3); "
    "    while i < 20 do "
    "      i := i + 1; "
    "      var o: IFresh3349B := h; "
    "      Console.Write(o.Fresh()); "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + churn.Length - churn.Length + 1; "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C3349B;";

// ===========================================================================
// (C) §18.5 interpolation hole through interface dispatch — the string twin
// of the box-residual sibling at one indirection deeper.
// ===========================================================================
const char* kInterfaceInterpolationProducer =
    "module M3349C; "
    "public interface IFresh3349C "
    "  method Fresh(): Object; "
    "end IFresh3349C; "
    "public class H3349IfaceC: IFresh3349C "
    "  public S: string := \"h\"; "
    "  public method Fresh(): Object => \"x\" + S; "
    "end H3349IfaceC; "
    "public class C3349C "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    using var h := new H3349IfaceC(); "
    "    h.S := \"s\" + \"elf\".Substring(0, 3); "
    "    while i < 20 do "
    "      i := i + 1; "
    "      var o: IFresh3349C := h; "
    "      var s: string := $\"[{o.Fresh()}]\"; "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + s.Length - s.Length + 1; "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C3349C;";

// ===========================================================================
// (D) Debug.WriteLine through interface dispatch — debug sibling, special-
// lowered path (mirrors shape G of the #2879 file).
// ===========================================================================
const char* kInterfaceDebugWriteLineProducer =
    "module M3349D; import System.Diagnostics; "
    "public interface IFresh3349D "
    "  method Fresh(): Object; "
    "end IFresh3349D; "
    "public class H3349IfaceD: IFresh3349D "
    "  public S: string := \"h\"; "
    "  public method Fresh(): Object => \"x\" + S; "
    "end H3349IfaceD; "
    "public class C3349D "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    using var h := new H3349IfaceD(); "
    "    h.S := \"s\" + \"elf\".Substring(0, 3); "
    "    while i < 20 do "
    "      i := i + 1; "
    "      var o: IFresh3349D := h; "
    "      Debug.WriteLine(o.Fresh()); "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + churn.Length - churn.Length + 1; "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C3349D;";

// ===========================================================================
// (E) OVER-RELEASE ORACLE — interface dispatch to a method returning a STORED
// FIELD. `IFresh3349E.Fresh` resolves to `H3349IfaceE.Fresh` whose body is
// `S` (a borrow, NOT a +1 producer). The interface arm must NOT admit the
// call — the producer classification is per-concrete-method and the concrete
// method is not in objectStringReturnMethods_.
// ===========================================================================
const char* kInterfaceFieldReturnNoOverRelease =
    "module M3349E; "
    "public interface IFresh3349E "
    "  method Fresh(): Object; "
    "  method Producer(): Object; "
    "end IFresh3349E; "
    "public class H3349IfaceE: IFresh3349E "
    "  public S: string := \"h\"; "
    "  public method Fresh(): Object => S; "
    "  public method Producer(): Object => \"x\" + S; "
    "end H3349IfaceE; "
    "public class C3349E "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    using var h := new H3349IfaceE(); "
    "    h.S := \"self\"; "
    "    while i < 20 do "
    "      i := i + 1; "
    "      var o: IFresh3349E := h; "
    "      Console.WriteLine(o.Fresh()); "
    "      Console.WriteLine(o.Producer()); "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + churn.Length - churn.Length + 1; "
    "    end while; "
    "    if h.S.Length = 4 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C3349E;";

// ===========================================================================
// (F) NO-ADMIT ORACLE — TWO locally-compiled implementers of the same
// interface, only one of which is a +1 producer. The interface arm must
// NOT admit the call: dispatch could land on the non-producer override
// (H3349IfaceF2.Fresh, which returns the bound `S` — a borrow, NOT a
// producer) and over-release the field's storage. The "exactly one
// implementer" gate keeps the fix conservative. Main calls F2.Fresh via
// interface dispatch — the over-release scenario.
//
// ⚠ ORACLE NOTE: this test uses ONLY runAot + runAotCheckedLenient (no
// LSan) because the conservative gate ACCEPTS the +1 leak when the
// non-producer is dispatched (or even when the producer is dispatched, the
// conservative gate does not admit, so the callee's +1 leaks — bounded at
// one allocation per call). The "decline-with-reasons" direction: rather
// than over-release (UAF), accept a bounded leak on a multi-implementor
// interface. The test's contract is "no UAF" (runAotCheckedLenient
// catches a premature free); it does NOT assert no leak.
// ===========================================================================
const char* kInterfaceMultiImplementorNotAdmitted =
    "module M3349F; "
    "public interface IFresh3349F "
    "  method Fresh(): Object; "
    "end IFresh3349F; "
    "public class H3349IfaceF1: IFresh3349F "
    "  public S: string := \"one\"; "
    "  public method Fresh(): Object => \"x\" + S; "
    "end H3349IfaceF1; "
    "public class H3349IfaceF2: IFresh3349F "
    "  public S: string := \"two\"; "
    "  public method Fresh(): Object => S; "
    "end H3349IfaceF2; "
    "public class C3349F "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    using var h := new H3349IfaceF2(); "
    "    h.S := \"self\"; "
    "    while i < 20 do "
    "      i := i + 1; "
    "      var o: IFresh3349F := h; "
    "      Console.WriteLine(o.Fresh()); "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + churn.Length - churn.Length + 1; "
    "    end while; "
    "    if h.S.Length = 4 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C3349F;";

// ===========================================================================
// (G) CLASS-typed receiver — control test for the #2879 shape. The interface
// arm must NOT regress the direct-Object sibling: when the receiver is
// statically a class, the pre-existing direct-hit branch admits the call.
// ===========================================================================
const char* kClassReceiverControl =
    "module M3349G; "
    "public class H3349G "
    "  public S: string := \"h\"; "
    "  public method Fresh(): Object => \"x\" + S; "
    "end H3349G; "
    "public class C3349G "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    using var h := new H3349G(); "
    "    h.S := \"s\" + \"elf\".Substring(0, 3); "
    "    while i < 20 do "
    "      i := i + 1; "
    "      Console.WriteLine(h.Fresh()); "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + churn.Length - churn.Length + 1; "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C3349G;";

}  // namespace

// ===========================================================================
// (A) Console.WriteLine through interface dispatch — pre-fix leaked 760 B / 40.
// runAotChecked forces generated-code ASan + LSan; LSan forces exit 1 on any
// leak, so exitCode == 0 IS the leak assertion.
// ===========================================================================
BEZEL_TEST(codegen_object_temp_string_leak_interface_dispatch_3349,
           interface_console_writeline_producer_is_reclaimed_spec_28_9) {
    JitResult plain = runAot(kInterfaceConsoleWriteLineProducer);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult r = runAotChecked(kInterfaceConsoleWriteLineProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // pre-fix: 1 (LSan reports 760 B / 40)
}

// ===========================================================================
// (B) Console.Write through interface dispatch.
// ===========================================================================
BEZEL_TEST(codegen_object_temp_string_leak_interface_dispatch_3349,
           interface_console_write_producer_is_reclaimed_spec_28_9) {
    JitResult plain = runAot(kInterfaceConsoleWriteProducer);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult r = runAotChecked(kInterfaceConsoleWriteProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // pre-fix: 1
}

// ===========================================================================
// (C) §18.5 interpolation hole through interface dispatch — string twin of
// the §2879/§2865 box residual at one indirection deeper.
// ===========================================================================
BEZEL_TEST(codegen_object_temp_string_leak_interface_dispatch_3349,
           interface_interpolation_hole_producer_is_reclaimed_spec_18_5) {
    JitResult plain = runAot(kInterfaceInterpolationProducer);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult r = runAotChecked(kInterfaceInterpolationProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // pre-fix: 1 (760 B / 40 — same shape)
}

// ===========================================================================
// (D) Debug.WriteLine through interface dispatch — proves the fix is
// symmetrical with Console, through the special-lowered debug path.
// ===========================================================================
BEZEL_TEST(codegen_object_temp_string_leak_interface_dispatch_3349,
           interface_debug_writeline_producer_is_reclaimed_spec_28_9) {
    JitResult plain = runAot(kInterfaceDebugWriteLineProducer);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult r = runAotChecked(kInterfaceDebugWriteLineProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (E) OVER-RELEASE ORACLE — an interface dispatch to a method returning a
// STORED FIELD must NOT be admitted (the field is borrowed, not produced).
// The lenient run (LSan off, ASan on) catches UAF if the interface arm
// over-admits.
// ===========================================================================
BEZEL_TEST(codegen_object_temp_string_leak_interface_dispatch_3349,
           interface_field_return_does_not_over_release_spec_28_3) {
    JitResult plain = runAot(kInterfaceFieldReturnNoOverRelease);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult lenient = runAotCheckedLenient(kInterfaceFieldReturnNoOverRelease);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(lenient));
    BEZEL_EXPECT_TRUE(lenient.ran);
    BEZEL_EXPECT_EQ(lenient.exitCode, 0);   // no UAF on the field
    JitResult r = runAotChecked(kInterfaceFieldReturnNoOverRelease);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);             // and no leak either
}

// ===========================================================================
// (F) NO-ADMIT ORACLE — multi-implementor interface (one producer, one not)
// must NOT be admitted. The dispatch could land on the non-producer override
// and over-release the bound `S` storage. The lenient runAotCheckedLenient
// oracle catches a premature free as a heap-use-after-free. The test calls
// F2.Fresh (a borrow) via interface dispatch — if the predicate HAD admitted
// it, the extra discharge would be a UAF on the bound `h.S`. The conservative
// gate prevents this.
//
// Note: NO runAotChecked (LSan) assertion here — the conservative direction
// accepts the bounded leak (see header comment on the program). The oracle
// is runAot (clean exit) + runAotCheckedLenient (no UAF).
// ===========================================================================
BEZEL_TEST(codegen_object_temp_string_leak_interface_dispatch_3349,
           interface_multi_implementor_does_not_over_release_spec_28_3) {
    JitResult plain = runAot(kInterfaceMultiImplementorNotAdmitted);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult lenient = runAotCheckedLenient(kInterfaceMultiImplementorNotAdmitted);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(lenient));
    BEZEL_EXPECT_TRUE(lenient.ran);
    BEZEL_EXPECT_EQ(lenient.exitCode, 0);   // no UAF on the field
}

// ===========================================================================
// (G) CLASS-typed receiver — the §2879 direct-Object sibling. The interface
// arm must not regress the existing pre-pass: this shape is admitted by the
// direct-hit branch and the leak is closed by the #2879 fix itself.
// ===========================================================================
BEZEL_TEST(codegen_object_temp_string_leak_interface_dispatch_3349,
           class_receiver_control_does_not_regress_spec_28_9) {
    JitResult plain = runAot(kClassReceiverControl);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult r = runAotChecked(kClassReceiverControl);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
