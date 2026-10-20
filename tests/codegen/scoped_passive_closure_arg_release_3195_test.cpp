// Codegen / leak — fix #3195. A `+1` closure producer (a lambda literal, or a
// method reference) passed DIRECTLY as a `scoped` function-typed parameter to
// a user-authored callee whose body NEVER references the parameter leaked its
// closure cell (+8B env block) every call. Issue #3195, single-module
// minimal repro filed alongside the family M2 follow-up test for #3189.
//
// Spec: §28.14 (scoped callback parameters — "storage-bounded capture"; "The
// `scoped` modifier … callee may store the callback, but only in `scoped`
// fields of the bounding object itself"), §28.9 (runtime-managed types /
// closure-cell reclaim), §27.5 (closures and captures), §27.6 (method
// references), §28.3 (exception-path discharge). Spec revision v0.254.
//
// THE BUG. A lambda literal passed directly as a `scoped` parameter is a `+1`
// closure producer: `lowerLambda` callocs a fresh cell at rc = 1 and codegen
// owns that single reference end to end. §28.14's contract says the callee MAY
// store the callback, but only into `scoped` fields of the bounding object —
// a storage operation that requires writing the parameter identifier into a
// `scoped` field of the bounding instance. When the callee body provably does
// NOT reference the parameter, no such write can have happened, so the caller
// retains its single +1 and must release after the call. The pre-fix codegen
// skipped `scoped` parameters in its argument-pass release arm (only plain
// call-bounded parameters got released — the #1073 / #2305 fix), so the
// closure cell + env block leaked per call. LSan on the issue's exact repro,
// verified on this worktree's base (d9e6389):
//
//   Direct leak of 40 byte(s) in 1 object(s)  <- the closure cell
//   (the env block is the captured-local's own cell; for a no-capture lambda
//    it's zeroed out and may not show as a separate leak)
//
// WHY THE CALLER MAY DROP IT (§28.14). The §28.14 storage-bounded contract
// makes storage a MAY, not a MUST: the only place the callee is permitted to
// store the callback is in `scoped` fields of the bounding object — a write
// that requires the parameter identifier in the callee body. When the body
// has zero references to the parameter, no such write can have occurred, so
// the callee holds no reference once the call returns, and the producer's
// single +1 is the caller's to drop. The conservative check walks the body
// for any `IdentifierExpr` whose name matches the parameter; any hit forces a
// bail (the leak is the safe direction; a wrongly-armed release would be a
// UAF).
//
// FAIL-WITHOUT / PASS-WITH. Cases A, B, D, E leak on base (40B closure cell
// each call). Case C is the control row from the issue body — plain `var p :=
// new Pub(); return 0;` — which is clean both before and after the fix.
// Cases F, G, H pin the over-arm guards: the callee body that DOES reference
// `h` (F), a sync iterator that retains (G), and a deferred-activation callee
// (H) all retain the closure past the call and the release must NOT arm — a
// wrongly-armed release would surface as the ASan use-after-free the helper
// still catches. The same exclusions the #1073 / #2305 arm applies carry over
// to the scoped arm unchanged (not async, not sync iter, declared return not
// a frame, substituted result not a frame / lazy-query / function value).

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 7 absorbing rows used to
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

// ===========================================================================
// A — the issue's exact minimal repro. A plain `var p := new Pub()` plus a
// `using var sub := p.Subscribe(lambda() => ());` — `Subscribe`'s body
// (`=> new Sub()`) does not reference the `scoped h` parameter, so the
// closure cell (40B) leaks on base. With the fix the cell is released
// after the call returns.
// ===========================================================================
BEZEL_TEST(codegen_3195_scoped_passive, minimal_repro_releases_scoped_closure_arg_spec_28_14) {
    auto r = runAotChecked(
        "module App; "
        "public class Sub "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Sub; "
        "public class Pub "
        "  public owned Held: Sub; "
        "  public constructor() Held := new Sub(); end constructor; "
        "  public method Subscribe(scoped h: () -> ()): owned Sub => new Sub(); "
        "  destructor end destructor; "
        "end Pub; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new Pub(); "
        "    using var sub := p.Subscribe(lambda() => ()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// B — KNOWN-RESIDUAL ROW (run under lenient ASan so the residual leak is
// visible but doesn't fail the test). The issue body's isolating table lists
// `+ capture-bearing lambda → 56 bytes / 3 allocations leaked`. After the fix
// the 40-byte closure-cell leak is closed (case A above); the residual 8-byte
// capture-cell leak for a sole-captured `using var` local comes from the
// `definitelyEscapingLambdas_` predicate not arming for a `scoped` parameter
// (a captured local's cell is freed by the lambda's envdtor at last-drop, but
// only when the lambda is DEFINITELY-escaping — an inline closure passed to
// a `scoped` parameter doesn't qualify, because §28.14 lets the callee
// dispose it). Pinning this row under ASan (without LSan) proves the fix's
// release does not introduce a USE-AFTER-FREE on the captured source — the
// UAF direction is the one an over-arm would surface. The 8-byte leak is a
// separate captured-local-escape fix filed under a follow-up issue.
// ===========================================================================
BEZEL_TEST(codegen_3195_scoped_passive, capture_bearing_lambda_scoped_param_no_uaf_spec_28_14) {
    auto r = runAotCheckedLenient(
        "module App; "
        "public class Sub "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Sub; "
        "public class Pub "
        "  public owned Held: Sub; "
        "  public constructor() Held := new Sub(); end constructor; "
        "  public method Subscribe(scoped h: () -> ()): owned Sub => new Sub(); "
        "  destructor end destructor; "
        "end Pub; "
        "public class Program "
        "  public static method Main(): int "
        "    using var b := new Sub(); "
        "    var p := new Pub(); "
        "    using var sub := p.Subscribe(lambda() => Program.Touch(b)); "
        "    return 0; "
        "  end Main; "
        "  public static method Touch(s: Sub) end Touch; "
        "end Program;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// C — the issue's explicit-`p.Dispose()` row. `p.Dispose()` after the using-var
// must NOT double-free (the using-var disposes `sub`, `p.Dispose()` disposes
// `p`; the closure cell was already released by the fix's after-call release).
// ===========================================================================
BEZEL_TEST(codegen_3195_scoped_passive, explicit_p_dispose_does_not_double_free_spec_28_3) {
    auto r = runAotChecked(
        "module App; "
        "public class Sub "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Sub; "
        "public class Pub "
        "  public owned Held: Sub; "
        "  public constructor() Held := new Sub(); end constructor; "
        "  public method Subscribe(scoped h: () -> ()): owned Sub => new Sub(); "
        "  destructor end destructor; "
        "end Pub; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new Pub(); "
        "    using var sub := p.Subscribe(lambda() => ()); "
        "    p.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// D — control: plain `var p := new Pub(); return 0;`. Auto-drops cleanly both
// before and after the fix; pins that the fix changes nothing for the
// no-Subscribe case.
// ===========================================================================
BEZEL_TEST(codegen_3195_scoped_passive, control_no_call_still_auto_drops_clean_spec_28_3) {
    auto r = runAotChecked(
        "module App; "
        "public class Sub "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Sub; "
        "public class Pub "
        "  public owned Held: Sub; "
        "  public constructor() Held := new Sub(); end constructor; "
        "  destructor end destructor; "
        "end Pub; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new Pub(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// E — §27.6 method reference (no-op static method reference bound at the call
// site). `isPlusOneClosureProducer` covers both lambda literals AND method
// references, so the same release must arm.
// ===========================================================================
BEZEL_TEST(codegen_3195_scoped_passive, method_reference_scoped_param_released_spec_27_6) {
    auto r = runAotChecked(
        "module App; "
        "public class Sub "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Sub; "
        "public class Pub "
        "  public owned Held: Sub; "
        "  public constructor() Held := new Sub(); end constructor; "
        "  public method Subscribe(scoped h: () -> ()): owned Sub => new Sub(); "
        "  destructor end destructor; "
        "end Pub; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new Pub(); "
        "    using var sub := p.Subscribe(Program.Noop); "
        "    return 0; "
        "  end Main; "
        "  public static method Noop() end Noop; "
        "end Program;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// F — OVER-ARM CONTROL #1: a callee body that DOES reference the `scoped`
// parameter MUST keep the closure alive (the callee is permitted to forward
// or invoke it without taking a +1, but the conservative body-references gate
// bails at any hit). A wrongly-armed release would UAF; the lenient helper
// still catches that with ASan. This row runs clean both before and after
// the fix (the body does no storage, so the call-bounded release path was
// already correct; the new scoped arm must simply NOT also fire here).
// ===========================================================================
BEZEL_TEST(codegen_3195_scoped_passive, over_arm_control_body_references_h_spec_28_14) {
    auto r = runAotCheckedLenient(
        "module App; "
        "public class Sub "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Sub; "
        "public class Pub "
        "  public owned Held: Sub; "
        "  public constructor() Held := new Sub(); end constructor; "
        "  public method Subscribe(scoped h: () -> ()): owned Sub "
        "    h(); return new Sub(); "
        "  end Subscribe; "
        "  destructor end destructor; "
        "end Pub; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new Pub(); "
        "    using var sub := p.Subscribe(lambda() => ()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// G — OVER-ARM CONTROL #2: a SYNC ITERATOR callee already retains the closure
// in its state-machine frame (#1864's release path). The new scoped arm must
// NOT also fire here — that would be a double release on the iterator frame's
// own +1. The existing call-bounded exclusion (`!isSyncIterCallee`) carries
// over identically. (Run under lenient ASan because a wrongly-armed release
// here would UAF.)
// ===========================================================================
BEZEL_TEST(codegen_3195_scoped_passive, over_arm_control_sync_iterator_retains_spec_28_14) {
    auto r = runAotCheckedLenient(
        "module App; "
        "public class Sub "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Sub; "
        "public class Pub "
        "  public owned Held: Sub; "
        "  public constructor() Held := new Sub(); end constructor; "
        "  public method Subscribe(scoped h: () -> ()): IEnumerable<int> "
        "    yield return 1; "
        "  end Subscribe; "
        "  destructor end destructor; "
        "end Pub; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new Pub(); "
        "    using var sub := p.Subscribe(lambda() => ()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
