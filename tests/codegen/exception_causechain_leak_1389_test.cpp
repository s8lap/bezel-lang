// Codegen + runtime — reclaiming the #811 CauseChain snapshot on exception
// disposal (issue #1389 item 1; §31.4 / §28.9, spec v0.228).
//
// §31.4 (Bezel_Language_Spec.md, v0.228): "the `(message, cause)` constructor
// BORROWS `cause` for the duration of the call and copies its type name,
// message, stack-trace text, and its own `CauseChain` into the new
// exception's `CauseChain` ... The cause *instance* is never retained ...
// `public readonly property CauseChain: Array<ExceptionInfo>;`" — the new
// exception OWNS this snapshot array. §31.1 ("Exception ownership"): "`new
// SomeException(...)` produces an owned class instance ... ownership
// transfers to the runtime's exception machinery, which disposes the
// exception after the matching catch arm completes" — and §28 (Resource
// Management) requires every owned heap object to be disposed exactly once,
// which for an exception's owned CauseChain snapshot means the snapshot's
// backing array (and the strings it retained) must be reclaimed when the
// exception itself is.
//
// THE BUG (#1389 item 1). `bz_exc_causechain_link` (bezel_runtime_itable.cpp)
// builds a fresh `Array<ExceptionInfo>` and retains a +1 on every string it
// copies in; the exception's disposer (`bz$exDisposeMsg`,
// setupBuiltinExceptions in LlvmBackend_Setup.cpp) released Message and
// ParamName but never touched CauseChain@24 — so every disposed exception
// constructed WITH a cause leaked its snapshot array and every retained
// string inside it. Confirmed still present on main as of the #1389 triage:
// reverting the fix below reproduces the leak under LSan (see the commit
// message for the exact before/after byte counts).
//
// THE FIX. CauseChain is now reclaimed at the TERMINAL reclaim site
// (`Emitter::emitDisposeAndReclaim`, LlvmBackend_Disposal.cpp) via the new
// runtime helper `bz_exc_reclaim_causechain`, gated on the object's vtable
// being one of the built-in exception vtables — NOT inside the shared,
// possibly-multiply-dispatched vtable Dispose (`bz$exDisposeMsg`), which is
// where an earlier attempt reportedly produced a use-after-free (see the
// comments at both sites). The reclaim site runs exactly once per object,
// strictly after every live borrow/read of it, by ownership discipline.
//
// OBSERVABILITY. `runAotChecked` links the generated code against the
// ASan/LSan-instrumented runtime; a leak (or a UAF/double-free the fix could
// introduce) flips `r.ran` to false. The loop count (40, per the #1389 triage
// protocol) amplifies the leak so it is unambiguous under LSan.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// #1389 item 1 repro (using var / scope-end dispose path, §28.10): each loop
// iteration builds a fresh ArgumentException `inner`, chains it as the cause
// of a fresh `Exception outer` (the #811 snapshot constructor), then lets
// both go out of scope (reverse order per §28.4: outer first, then inner).
// Before the fix, `outer`'s CauseChain snapshot (and the strings it retained)
// leaked on every iteration. Returns 0.
const char* kCauseChainUsingVarLoop =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 40 do "
    "      using var inner := new ArgumentException(\"inner-msg\"); "
    "      using var outer := new Exception(\"outer-msg\", inner); "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// #1389 item 1 repro (catch-arm dispose path, §31.1/§31.2.5): `inner` is an
// owned using-var (never thrown); `outer` chains it as a cause, is thrown,
// caught, and suppressed — the runtime's dispose-after-arm reclaims `outer`.
// Exercises the SAME terminal reclaim site via a different caller
// (emitDisposeAndReclaim(currentCaughtExc_), not the using-var scope-end
// caller) — both must be covered by the fix. Returns 0.
const char* kCauseChainCatchArmLoop =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 40 do "
    "      using var inner := new ArgumentException(\"inner-msg\"); "
    "      try "
    "        throw new Exception(\"outer-msg\", inner); "
    "      catch "
    "        var e: Exception => (); "
    "      end; "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// No-regression control: a PLAIN exception (no cause) has a null CauseChain
// slot — bz_exc_reclaim_causechain must no-op on it, not crash or misread
// garbage at offset 24. This loop leaked nothing even before the fix (the
// #1033 Message-release fix already covers it); it stays clean after.
const char* kPlainExceptionNoCauseLoop =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 40 do "
    "      using var e := new ArgumentException(\"plain-msg\"); "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #1389 item 1 — the CauseChain snapshot is reclaimed exactly once, with
// no leak and no UAF/double-free, on the using-var scope-end dispose path.
// =====================================================================

BEZEL_TEST(codegen_exception_causechain_leak_1389,
           causechain_reclaimed_using_var_scope_end_31_4_28_9) {
    JitResult r = runAotChecked(kCauseChainUsingVarLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));

    // Clean run under LSan (the #1389 leak would flip this false), and no
    // UAF/double-free from the fix (also flips r.ran false).
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// #1389 item 1 — same reclaim, exercised via the catch-arm dispose path
// (§31.1's dispose-after-arm) instead of using-var scope-end.
// =====================================================================

BEZEL_TEST(codegen_exception_causechain_leak_1389,
           causechain_reclaimed_catch_arm_dispose_31_1_31_2_5) {
    JitResult r = runAotChecked(kCauseChainCatchArmLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// No-regression control — a plain (non-chained) exception's null CauseChain
// slot is handled as a no-op (was already leak-free via #1033; must stay so).
// =====================================================================

BEZEL_TEST(codegen_exception_causechain_leak_1389,
           plain_exception_no_cause_still_clean_31_4) {
    JitResult r = runAotChecked(kPlainExceptionNoCauseLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
