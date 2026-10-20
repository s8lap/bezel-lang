// Codegen / leak — a `string` LOCAL bound to a +1 producer and then passed as a
// plain constructor argument to `new Box(s)` (Box holds a `_s: string` field)
// leaked its single reference when Box was auto-disposed at scope end. Issue
// #2023.
//
// Spec: §28.3 (local-variable ownership and borrows; the scope-end auto-drop),
// §28.9 (runtime-managed types; the string refcount discipline), §28.4 step 2
// (a class's compiler-generated Dispose releases its runtime-managed string
// fields — the field's INDEPENDENT +1, retained at store), §22.3 (parameter
// modifiers — a plain parameter takes a call-bounded borrow), §11 (classes and
// fields). Spec revision v0.204.
//
// THE BUG. A `var` string local bound to a +1 producer (a user-method string
// return, a numeric `.ToString()`, a `+` concat, an interpolation, …) owns
// that single reference. Passing the local as a plain constructor argument to
// `new Box(s)` where Box has a `_s: string` field causes the ctor's
// lowerStringFieldStore to RETAIN the value into the field (the #429 argpass
// cardinal canary — the field holds an INDEPENDENT +1, so it survives past
// the local's scope). At scope end Box.Dispose releases the field's +1 (rc 2→1
// for the shared underlying string), and the local's own +1 must ALSO be
// released (rc 1→0, freed exactly once). The StringEscapeWalker is what gates
// a local's scope-end release, and its NewExpr arm conservatively treated
// EVERY arg as ESCAPED (it does not consult walkCallArgs). An escaping local
// is excluded from the scope-end release, so the producer's +1 was never
// dropped: it leaked (32-byte header + data buffer), once per such local.
//
// Distinct from #1994 (which is the WriteLine borrow-arg leak in
// StringEscapeWalker's Console builtin path). This is the NewExpr arm's
// analogous gap: a constructor is compiled under the SAME string refcount
// discipline as a resolved user method (the #1430 relaxation), so a plain
// ctor argument is a CALL-BOUNDED borrow — not an escape — and the local
// must stay scope-end-release eligible.
//
// THE FIX (LlvmBackend_Setup.cpp, StringEscapeWalker::walkNewArgs + the
// NewExpr arm in expr()): a string local passed in a plain (no-modifier,
// positional) parameter position of a RESOLVED constructor (exactly one
// ctor of the target type matches by arity) with a plain parameter at the
// index is a CALL-BOUNDED borrow, NOT an escape — exactly the #1430
// user-method-call relaxation extended to the constructor-call form.
// Gated on stringReleaseMode (the #1997 narrowing) so the shared
// dict/grouping/borrowing-Array walks keep their conservative ESCAPE for a
// NewExpr arg.
//
// NO owned-sink hazard. `owned` is inadmissible on a `string` parameter
// (§28.1 — strings are runtime-managed reference types with no disposal
// obligation to transfer), so there is no string owned-sink that could
// double-free. The only "adopt" for a string is a field store (which mints
// its own +1, #429) or an independent escape position (return/new/assign/
// lambda-capture), all of which the expr() walk visits in their own arms and
// which independently taint the local — the transfer-guard regression tests
// below pin that the ctor relaxation does not override them.
//
// OBSERVABILITY. runAotChecked links + runs the generated code under ASan/LSan;
// a leaked string OR a double-free/UAF (over-releasing a value the ctor still
// borrows, or one that escapes) flips r.ran to false. The exit code pins the
// value via `.Length`, so a premature free would corrupt it as well as flip
// r.ran.
//
// Two kinds of case:
//   * LEAK REPROS (A,B,C,D,E,F) — LSan-leak on base 35e854b, LSan-clean with
//     the fix (fail-without / pass-with).
//   * DOUBLE-FREE / TRANSFER GUARDS (G,H) — LSan-clean BOTH before and after
//     the fix: a local ctor-arg'd AND returned (the return transfers the +1,
//     which must still exclude the local from the scope-end release), and a
//     direct producer argument (no local — the ctor retains the +1 fresh arg
//     and Box.Dispose releases it). These prove the fix introduces no
//     double-free and leaves the direct-producer path untouched.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 8 checked rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics, and then RETURNED from the row on
// it. That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a silent
// vacuous pass: on a box whose clang cannot link the ASan binary, all
// 8 rows returned WITHOUT asserting and the suite reported a clean PASS
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

// =====================================================================
// LEAK REPROS — LSan-leak on base 35e854b, LSan-clean with the fix.
// =====================================================================

// (A) The #2023 repro: a USER-METHOD string-return producer bound to a local,
// then passed to `new Box(s)`. The producer's +1 (36 bytes) leaked on base.
const char* kUserMethodReturnLocalToCtor =
    "module M; class Box "
    "  public _s: string; "
    "  public constructor(s: string) "
    "    _s := s; "
    "  end constructor; "
    "end Box; "
    "class C "
    "  public static method Render(): string "
    "    var n := 7; "
    "    return \"n=\" + n.ToString(); "
    "  end Render; "
    "  public static method Main(): int "
    "    var s := C.Render(); "
    "    var b := new Box(s); "          // ctor retains s (rc=2); s must still be released
    "    return b._s.Length; "           // "n=7".Length = 3
    "  end Main; "
    "end C;";

// (B) The #2023 repro WITHOUT WriteLine — proves the leak is independent of
// #1994's WriteLine path. A `+`-concat producer local passed straight to the
// Box ctor leaks identically on base.
const char* kConcatLocalToCtor =
    "module M; class Box "
    "  public _s: string; "
    "  public constructor(s: string) "
    "    _s := s; "
    "  end constructor; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    "    var n := 4; "
    "    var s := \"v=\" + n.ToString(); "
    "    var b := new Box(s); "
    "    return b._s.Length; "           // "v=4".Length = 3
    "  end Main; "
    "end C;";

// (C) The original #1994-lane F_field probe: producer → WriteLine → ctor. On
// base, BOTH the WriteLine leak (#1994) AND the ctor leak (#2023) fire — the
// ctor relaxation alone closes the residual leak that #1994 left. Pre-#1994
// the leak was 72 bytes; post-#1994 pre-#2023 it was 36; with both fixes 0.
const char* kWriteLineThenCtor_FFieldProbe =
    "module M; class Box "
    "  public _s: string; "
    "  public constructor(s: string) "
    "    _s := s; "
    "  end constructor; "
    "end Box; "
    "class C "
    "  public static method Render(): string "
    "    var n := 7; "
    "    return \"n=\" + n.ToString(); "
    "  end Render; "
    "  public static method Main(): int "
    "    var s := C.Render(); "
    "    Console.WriteLine(s); "         // #1994: borrow (s stays release-eligible)
    "    var b := new Box(s); "          // #2023: borrow (s stays release-eligible)
    "    return b._s.Length; "           // "n=7".Length = 3
    "  end Main; "
    "end C;";

// (D) An INTERPOLATION producer local passed to the ctor. Same escape
// false-positive on a non-`+` producer.
const char* kInterpolationLocalToCtor =
    "module M; class Box "
    "  public _s: string; "
    "  public constructor(s: string) "
    "    _s := s; "
    "  end constructor; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    "    var n := 9; "
    "    var s := $\"x={n}\"; "
    "    var b := new Box(s); "
    "    return b._s.Length; "           // "x=9".Length = 3
    "  end Main; "
    "end C;";

// (E) A ToString producer local passed to the ctor. Same producer-agnostic
// leak on base — the gap is in the NewExpr arm, not any producer path.
const char* kToStringLocalToCtor =
    "module M; class Box "
    "  public _s: string; "
    "  public constructor(s: string) "
    "    _s := s; "
    "  end constructor; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    "    var n := 42; "
    "    var s := n.ToString(); "
    "    var b := new Box(s); "
    "    return b._s.Length; "           // "42".Length = 2
    "  end Main; "
    "end C;";

// =====================================================================
// DOUBLE-FREE / TRANSFER GUARDS — LSan-clean BOTH before and after the fix.
// =====================================================================

// (F) Two Boxes from the SAME producer local — rc=3 after both ctors retain;
// each Box.Dispose releases one +1 (rc 3→2→1) and the local's scope-end
// release frees the last (rc 1→0). On base the local's +1 is never released
// (escape over-conservative), so the underlying string leaks (rc=1 at exit).
const char* kCtorThenReturnTransfer =
    "module M; class Box "
    "  public _s: string; "
    "  public constructor(s: string) "
    "    _s := s; "
    "  end constructor; "
    "end Box; "
    "class C "
    "  public static method Emit(): string "
    "    var s := \"v=\" + 5.ToString(); "
    "    var b := new Box(s); "          // ctor retains (rc=2)
    "    Console.WriteLine(b._s.Length); "
    "    return s; "                      // escape: +1 transferred out (rc=2 → 1 after Box.Dispose in Emit)
    "  end Emit; "
    "  public static method Main(): int "
    "    var r := C.Emit(); "
    "    return r.Length; "              // "v=5".Length = 3, freed once in Main
    "  end Main; "
    "end C;";

// (G) A producer local that is ctor'd AND ESCAPES via `return`. The return
// transfers the local's +1 to the caller, so the local must STAY excluded
// from the scope-end release even though it is also ctor'd — otherwise the
// returned reference would be double-freed. The #2023 relaxation only relaxes
// the ctor argument position; the independent return-escape keeps s tainted.
// Clean both before and after the fix. Main consumes the returned "v=5".
const char* kTwoBoxesFromSameString =
    "module M; class Box "
    "  public _s: string; "
    "  public constructor(s: string) "
    "    _s := s; "
    "  end constructor; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    "    var s := \"v=\" + 5.ToString(); "
    "    var b1 := new Box(s); "         // retain: rc=2
    "    var b2 := new Box(s); "         // retain: rc=3
    "    return b1._s.Length + b2._s.Length; "  // "v=5".Length + "v=5".Length = 6
    "  end Main; "
    "end C;";

// (H) DIRECT producer argument (no local): a fresh concat handed straight to
// the ctor. The ctor retains the +1 fresh arg into the field; Box.Dispose
// releases it at scope end. The #2023 relaxation is scoped to a bare string-
// LOCAL identifier arg, so it does not touch this — it stays correct (no leak,
// no double-free) exactly as on base.
const char* kDirectProducerArg =
    "module M; class Box "
    "  public _s: string; "
    "  public constructor(s: string) "
    "    _s := s; "
    "  end constructor; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    "    var n := 7; "
    "    var b := new Box(\"n=\" + n.ToString()); "  // direct +1 producer: ctor retains
    "    return b._s.Length; "           // "n=7".Length = 3
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// LEAK REPROS — fail-without (base leaks) / pass-with (fix releases once).
// §28.3 / §28.9 / §28.4 / §22.3 (v0.204). Cases A,B,C,D,E,F.
// =====================================================================
BEZEL_TEST(owned_class_string_field_ctor_arg_leak_2023,
           user_method_return_local_ctor_arg_lsan_clean_spec_28_9) {
    JitResult r = runAotChecked(kUserMethodReturnLocalToCtor);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // producer +1 released once (leaked pre-fix)
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // "n=7".Length
}

BEZEL_TEST(owned_class_string_field_ctor_arg_leak_2023,
           concat_local_ctor_arg_lsan_clean_spec_4_6) {
    JitResult r = runAotChecked(kConcatLocalToCtor);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // ctor retains s; s released once (leaked pre-fix)
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // "v=4".Length
}

BEZEL_TEST(owned_class_string_field_ctor_arg_leak_2023,
           writeline_then_ctor_residual_leak_closed_spec_28_3) {
    JitResult r = runAotChecked(kWriteLineThenCtor_FFieldProbe);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // #1994+#2023 together: 0 leaks
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // "n=7".Length
}

BEZEL_TEST(owned_class_string_field_ctor_arg_leak_2023,
           interpolation_local_ctor_arg_lsan_clean_spec_28_9) {
    JitResult r = runAotChecked(kInterpolationLocalToCtor);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // interpolation +1 released once
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // "x=9".Length
}

BEZEL_TEST(owned_class_string_field_ctor_arg_leak_2023,
           tostring_local_ctor_arg_lsan_clean_spec_4_6) {
    JitResult r = runAotChecked(kToStringLocalToCtor);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // ToString +1 released once
    BEZEL_EXPECT_EQ(r.exitCode, 2);  // "42".Length
}

BEZEL_TEST(owned_class_string_field_ctor_arg_leak_2023,
           two_boxes_from_same_string_lsan_clean_spec_28_9) {
    JitResult r = runAotChecked(kTwoBoxesFromSameString);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // rc=3 → 3 releases; freed exactly once (leaked pre-fix)
    BEZEL_EXPECT_EQ(r.exitCode, 6);  // "v=5".Length + "v=5".Length
}

// =====================================================================
// DOUBLE-FREE / TRANSFER GUARDS — LSan-clean BOTH before and after the fix.
// §28.1 / §28.3 / §28.9 (v0.204).
// =====================================================================
BEZEL_TEST(owned_class_string_field_ctor_arg_leak_2023,
           ctor_then_return_transfer_no_double_free_spec_28_3) {
    JitResult r = runAotChecked(kCtorThenReturnTransfer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // ctor'd + returned: +1 transferred, not double-freed
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // "v=5".Length
}

BEZEL_TEST(owned_class_string_field_ctor_arg_leak_2023,
           direct_producer_arg_untouched_spec_28_9) {
    JitResult r = runAotChecked(kDirectProducerArg);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // fresh +1 producer: ctor retains, Box.Dispose releases
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // "n=7".Length
}
