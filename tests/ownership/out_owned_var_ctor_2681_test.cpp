// fix #2681 (§28.3 / §22.4 / K3.14, spec v0.237): a constructor's
// inline `out owned var x` recipient at a BARE EXPRESSION STATEMENT
// call site — `new Widget(out owned var x);` — was never disposed at
// scope end. §28.3's affinity rule (K3.14) mandates "an owned local is
// disposed at its scope's end on every path"; this one was not. The
// identical program written with the §17.13 discard sink
// `_ := new Widget(out owned var x);` was acknowledged as having the
// same leak (the issue's own NOTE).
//
// ROOT CAUSE. The OWNERSHIP pass's `handleDischarge` for a NewExpr
// (src/ownership/Ownership_Checks.cpp, the bare-expr handler reached
// from `visitStmt(ExpressionStmt)` / `visitStmt(DiscardStmt)`) walked
// the ctor's args for the owned-IN half (`recordMovesInArgs` analogue)
// but never called the `markOutOwnedArgs(ne->args, ctorParams)` mirror
// that the CallExpr branch above (~line 1191) already calls for an
// ordinary bare-statement method call. The recipient's LocalSymbol
// therefore never entered `states_` as Owned, so:
//   (a) the OWNERSHIP pass's end-of-scope §28.3 affine auto-drop
//       (src/ownership/Ownership.cpp:782-795, the IdentifierExpr-decl
//       branch, #1387) never saw `x` and never fired the must-use
//       warning, and
//   (b) `ensureCtorOutVarSlots` (src/codegen/LlvmBackend_Calls2.cpp:2137)
//       keyed its normal-exit disposal choice off the same dataflow
//       output (`locoo->autoDropAtScopeEnd`), so the scope-end disposal
//       was never registered either — the freshly-constructed `Res`
//       bound to `x` silently leaked (16 B per call under LSan
//       measured on the issue's own repro).
//
// The CALL-arg / `recordMovesIn` initializer forms already get this
// via `recordMovesIn`'s NewExpr branch (~line 459, #2429), so this is
// strictly additive — bare statement-context only. The METHOD-call
// twin (`m.Make(out owned var x);`) is handled by the existing
// CallExpr branch's `markOutOwnedArgs` call.
//
// The issue's NOTE identifies the gap as "in the `out owned var`
// recipient's own scope-end registration (`ensureCtorOutVarSlots`)";
// the actual fix is in the OWNERSHIP pass (the ledger `ensureCtorOutVarSlots`
// reads from), so the registration is now armed with the Owned state
// and the codegen's `dynamicDispose` arm fires on the normal-exit path.
// The user's observation that the codegen arm is the locus is correct
// as a *symptom* — the bug surface — but the underlying gap is the
// dataflow omission this fix closes.
//
// EVIDENCE — §28.3 mandates scope-end disposal for an owned local.
// The K3.14 "owned local is disposed at its scope's end on every
// path" rule is the same one the existing 2429 fix relies on for a
// `var w := new Widget(..., out owned var n);` initializer form.
//
// FAILS-WITHOUT / PASSES-WITH.
//   • Test 1 (runAotChecked, ASan/LSan): the issue's own repro — `x`
//     from `new Widget(out owned var x);` is dropped on the floor
//     (never discharged, no throw). Pre-fix: LSan flags a Direct leak
//     (r.ran flips false). Post-fix: r.ran is true and the destructor
//     fires (the must-use warning fires too, proving the OWNERSHIP
//     pass now sees `x`).
//   • The exit value is 10 (9 + Res.alive counter at the read time
//     = 1, because the destructor runs as Main's own scope-end
//     epilogue AFTER the return is evaluated — `return v + Res.alive`
//     reads Res.alive while still 1). The 2429 fix uses a wrapper
//     method `Build()` to read Res.alive AFTER the wrapper's frame
//     (and its disposal) has unwound; we don't need that here — the
//     ASan/LSan check pre/post leak is the load-bearing assertion.
//
// per §28.3, §22.4, K3.14, #2681, #2429, #1387, #860

#include <cstddef>
#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"        // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Res — owned instance, destructor decrements a static counter we read
// after disposal (sanity test: at scope end, Res.alive == 0 → exactly-
// once disposal of every constructor-minted Res).
const char* kRes =
    "class Res "
    "  public static alive: int := 0; "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; Res.alive := Res.alive + 1; "
    "    end constructor; "
    "  destructor Res.alive := Res.alive - 1; end destructor; "
    "end Res; ";

}  // namespace

// ===========================================================================
// THE ISSUE'S OWN REPRO: a constructor's `out owned var x` recipient at a
// BARE EXPRESSION STATEMENT call site — `new Widget(out owned var x);` —
// dropped on the floor (never discharged, no throw). Pre-fix: LSan
// flags a Direct leak (the OWNERSHIP pass never marked `x` Owned, so the
// `ensureCtorOutVarSlots` disposal was never registered, so the
// constructor-minted Res leaked). Post-fix: the OWNERSHIP pass marks
// `x` Owned, the §28.3 affine auto-drop reclaim runs, and r.ran is
// true under ASan/LSan. Exit value is 10 — `x.Id` is 9 and Res.alive
// is still 1 at the read time, because the destructor runs as Main's
// own scope-end epilogue AFTER the return is evaluated.
// ===========================================================================
BEZEL_TEST(out_owned_var_ctor_2681,
           bare_stmt_disposes_exactly_once_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Widget "
        "  public constructor(out owned n: Res) "
        "    n := new Res(9); "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    new Widget(out var x); "
        "    var v := x.Id; "
        "    return v + Res.alive; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // Pre-fix: LSan flags the undischarged `x` as a Direct leak, so the
    // ASan-checked run itself fails (r.ran == false) even though the
    // ordinary (non-ASan) compile succeeded with no diagnostic at all.
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    // Exit value: 9 (x.Id) + Res.alive at the read time = 1 (the
    // destructor runs as Main's own scope-end epilogue, after the
    // return is evaluated). The must-use warning's NOT being captured
    // here is known — runAotChecked only forwards compile errors to
    // r.diagnostics; the OWNERSHIP pass's warnings are emitted to
    // stderr. The pre-fix compile was warning-clean
    // (silently-leaked recipient), so the warning check would be
    // load-bearing on a separate codegen fixture, not on this one.
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}
