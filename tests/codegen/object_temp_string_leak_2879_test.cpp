// Codegen — an Object-typed METHOD CALL whose body provably always returns a
// fresh +1 string is itself a +1 producer. Consumed at `Console.Write`/`Write`
// (per stdlib §2.1, `value: Object`) or at a §18.5 interpolation hole, the
// callee's +1 is now DISCHARGED ONCE by `lowerObjectToString`'s RETAIN+release
// pair (#2846 — the render's +1), but the callee's OWN +1 (the fresh string the
// body minted) is orphaned unless an extra discharge fires at the call site.
// Without that discharge the string leaks 38 B/call, unbounded in a loop (760 B
// in 40 allocations over 20 iterations, the measured shape on 564b6d1).
//
// Spec (revision v0.240):
//   §8.5 "Rendering values for `Console.Write`/`WriteLine` and string
//     interpolation" — class-typed values render via virtual ToString dispatch;
//     for a string receiver `lowerObjectToString` returns the retained
//     receiver.
//   §8.6 "Boxing" — value types in Object slots box; the present shape is a
//     STRING, which is NOT a value type — bare bz_string* in an Object slot.
//   §18.5 "Interpolated strings" — each hole renders by §8.5.
//   §28.3 "Local variables, ownership, and borrows" — an un-bound temporary
//     has no owner; #2865 is its box residual.
//   §28.9 "Runtime-managed types" — the +1 ownership contract for strings.
//
// Shape coverage — every consumption position the fix closes, plus the
// over-release oracle shapes the fix MUST NOT break:
//   (1) Console.WriteLine(h.Fresh())         — the issue's primary shape
//   (2) Console.Write(h.Fresh())             — sibling without trailing \n
//   (3) $"[{h.Fresh()}]"                     — §18.5 interpolation hole
//   (4) a Block-bodied method body that always returns a +1 string producer
//   (5) an arrow-bodied method that returns a non-producer (a stored field)
//       — must NOT over-release the bound `S` storage
//   (6) a stored Object local pointing at a string borrow — must NOT over-release
//   (7) `Debug.WriteLine(h.Fresh())`         — debug sibling, special-lowered
//
// ⚠ ORACLE DISCIPLINE (mirrors box_temp_release_and_interp_2865_test.cpp):
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

// #3733 toolchain-SKIP burn-down. This file's 7 checked rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics, and then RETURNED from the row on
// it. 2 of them ran their assertion block only inside an
// `if (!wasSkipped(...))` — the same SKIP test in POSITIVE polarity — so
// those rows skipped their assertions outright as well.
// That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a silent
// vacuous pass: on a box whose clang cannot link the ASan binary, all
// 7 rows returned WITHOUT asserting and the suite reported a clean PASS
// having compiled and run nothing. MEASURED on the pre-change tree with the
// harness's own knob (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): every row green.
//
// Every one of those guards becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position: the
// skip is now LOUD — the row FAILS — instead of being absorbed by a guard
// that reports a clean pass. The lenient legs' assertions additionally
// now RUN UNCONDITIONALLY rather than being skipped, which is the other half of
// the strengthening. Every substantive assertion in every row (r.ran and the
// exitCode pins) stays exactly where it was.

// ===========================================================================
// (A) The issue's primary repro, ASCII-clean for `console_writeline`.
// ===========================================================================
const char* kConsoleWriteLineProducer =
    "module M2879A; "
    "public class H2879A "
    "  public S: string := \"h\"; "
    "  public method Fresh(): Object => \"x\" + S; "
    "end H2879A; "
    "public class C2879A "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    using var h := new H2879A(); "
    "    h.S := \"s\" + \"elf\".Substring(0, 3); "
    "    while i < 20 do "
    "      i := i + 1; "
    "      Console.WriteLine(h.Fresh()); "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + churn.Length - churn.Length + 1; "
    "    end while; "
    "    Console.WriteLine(h.S); "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C2879A;";

// ===========================================================================
// (B) Sibling without the trailing newline — same path, narrower write.
// ===========================================================================
const char* kConsoleWriteProducer =
    "module M2879B; "
    "public class H2879B "
    "  public S: string := \"h\"; "
    "  public method Fresh(): Object => \"x\" + S; "
    "end H2879B; "
    "public class C2879B "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    using var h := new H2879B(); "
    "    h.S := \"s\" + \"elf\".Substring(0, 3); "
    "    while i < 20 do "
    "      i := i + 1; "
    "      Console.Write(h.Fresh()); "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + churn.Length - churn.Length + 1; "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C2879B;";

// ===========================================================================
// (C) Interpolation hole — the §18.5 sibling. Same string twin of #2865.
// ===========================================================================
const char* kInterpolationProducer =
    "module M2879C; "
    "public class H2879C "
    "  public S: string := \"h\"; "
    "  public method Fresh(): Object => \"x\" + S; "
    "end H2879C; "
    "public class C2879C "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    using var h := new H2879C(); "
    "    h.S := \"s\" + \"elf\".Substring(0, 3); "
    "    while i < 20 do "
    "      i := i + 1; "
    "      var s: string := $\"[{h.Fresh()}]\"; "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + s.Length - s.Length + 1; "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C2879C;";

// ===========================================================================
// (D) Block-bodied method that ALWAYS returns a +1 string producer (every
// ReturnStmt in the body passes isPlusOneStringProducer — runtime concat,
// interpolation, string `+`, .ToString(), slice, etc.). The Scanner walks
// the body and ANDs "every return is a producer" + ORs "any return exists".
// ===========================================================================
const char* kBlockBodyProducer =
    "module M2879D; "
    "public class H2879D "
    "  public S: string := \"h\"; "
    "  public method Fresh(): Object "
    "    if S.Length > 0 then return \"x\" + S; end; "
    "    return \"y\" + S; "
    "  end Fresh; "
    "end H2879D; "
    "public class C2879D "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    using var h := new H2879D(); "
    "    h.S := \"self\"; "
    "    while i < 20 do "
    "      i := i + 1; "
    "      Console.WriteLine(h.Fresh()); "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + churn.Length - churn.Length + 1; "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C2879D;";

// ===========================================================================
// (E) OVER-RELEASE ORACLE — a method returning a STORED FIELD is a borrow,
// NOT a +1 producer. The pre-pass must NOT admit it (isPlusOneStringProducer
// is false on a bare MemberAccessExpr / IdentifierExpr read). Without the
// pre-pass gate the extra discharge would over-release the bound `S` storage.
// ===========================================================================
const char* kFieldReturnNoOverRelease =
    "module M2879E; "
    "public class H2879E "
    "  public S: string := \"h\"; "
    "  public method Fresh(): Object => S; "
    "  public method Producer(): Object => \"x\" + S; "
    "end H2879E; "
    "public class C2879E "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    using var h := new H2879E(); "
    "    h.S := \"self\"; "
    "    while i < 20 do "
    "      i := i + 1; "
    "      Console.WriteLine(h.Fresh()); "
    "      Console.WriteLine(h.Producer()); "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + churn.Length - churn.Length + 1; "
    "    end while; "
    "    if h.S.Length = 4 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C2879E;";

// ===========================================================================
// (F) OVER-RELEASE ORACLE — a bound Object local pointing at a string borrow.
// The pre-pass must NOT admit the IDENTIFIER read (`var o: Object := s;
// Console.WriteLine(o);`) as a producer; `o`'s scope-end release absorbs the
// render's +1, and an extra discharge would be a use-after-free on `s`.
// ===========================================================================
const char* kBoundObjectNoOverRelease =
    "module M2879F; "
    "public class C2879F "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    while i < 20 do "
    "      i := i + 1; "
    "      var s: string := \"h\" + i.ToString(); "
    "      var o: Object := s; "
    "      Console.WriteLine(o); "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + churn.Length - churn.Length + s.Length - s.Length + 1; "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C2879F;";

// ===========================================================================
// (G) Debug.WriteLine sibling — #1032's special-lowered path, parallel to
// Console.WriteLine. Both must close the leak.
// ===========================================================================
const char* kDebugWriteLineProducer =
    "module M2879G; import System.Diagnostics; "
    "public class H2879G "
    "  public S: string := \"h\"; "
    "  public method Fresh(): Object => \"x\" + S; "
    "end H2879G; "
    "public class C2879G "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    using var h := new H2879G(); "
    "    h.S := \"s\" + \"elf\".Substring(0, 3); "
    "    while i < 20 do "
    "      i := i + 1; "
    "      Debug.WriteLine(h.Fresh()); "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + churn.Length - churn.Length + 1; "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C2879G;";

}  // namespace

// ===========================================================================
// (A) The Console.WriteLine shape — pre-fix leaked 760 B / 40 allocations.
// runAotChecked forces generated-code ASan + LSan; LSan forces exit 1 on any
// leak, so exitCode == 0 IS the leak assertion.
// ===========================================================================
BEZEL_TEST(codegen_object_temp_string_leak_2879,
           console_writeline_producer_is_reclaimed_spec_28_9) {
    JitResult plain = runAot(kConsoleWriteLineProducer);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult r = runAotChecked(kConsoleWriteLineProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // pre-fix: 1 (LSan reports 760 B / 40)
}

// ===========================================================================
// (B) Console.Write (no trailing \n) — the same path minus the newline write.
// ===========================================================================
BEZEL_TEST(codegen_object_temp_string_leak_2879,
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
// (C) §18.5 interpolation hole — the §2865/§2879 twin consumption position.
// ===========================================================================
BEZEL_TEST(codegen_object_temp_string_leak_2879,
           interpolation_hole_producer_is_reclaimed_spec_18_5) {
    JitResult plain = runAot(kInterpolationProducer);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult r = runAotChecked(kInterpolationProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // pre-fix: 1 (760 B / 40 — same shape)
}

// ===========================================================================
// (D) Block-bodied method that always returns a +1 string producer — proves
// the Scanner's "every ReturnStmt is a producer" gate admits the body shape.
// ===========================================================================
BEZEL_TEST(codegen_object_temp_string_leak_2879,
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
// (E) OVER-RELEASE ORACLE — a method returning a STORED FIELD must NOT be
// admitted (the field is borrowed, not produced). The lenient run (LSan off,
// ASan on) catches UAF if the pre-pass over-admits.
// ===========================================================================
BEZEL_TEST(codegen_object_temp_string_leak_2879,
           field_return_does_not_over_release_spec_28_3) {
    JitResult plain = runAot(kFieldReturnNoOverRelease);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult lenient = runAotCheckedLenient(kFieldReturnNoOverRelease);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(lenient));
    BEZEL_EXPECT_TRUE(lenient.ran);
    BEZEL_EXPECT_EQ(lenient.exitCode, 0);   // no UAF on the field
    JitResult r = runAotChecked(kFieldReturnNoOverRelease);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);             // and no leak either
}

// ===========================================================================
// (F) OVER-RELEASE ORACLE — a bound Object local must NOT be released by the
// producer arm. `o`'s scope-end release absorbs the render's +1; an extra
// discharge would UAF the underlying `s`.
// ===========================================================================
BEZEL_TEST(codegen_object_temp_string_leak_2879,
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
// (G) Debug.WriteLine sibling — proves the fix is symmetrical with Console.
// ===========================================================================
BEZEL_TEST(codegen_object_temp_string_leak_2879,
           debug_writeline_producer_is_reclaimed_spec_28_9) {
    JitResult plain = runAot(kDebugWriteLineProducer);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult r = runAotChecked(kDebugWriteLineProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
