// fix #2411 (§22.4 "Inline `out` declarations" / §22.3 "Parameter modifiers"
// / §28.3, spec v0.235): `new T(out owned var x)` — the constructor analogue
// of a plain method's inline `out owned var x` recipient — ICE'd
// UNCONDITIONALLY with no `and`/`or`/`?.` involved.
//
// ROOT CAUSE #1 (the ICE itself): sema (ExprTypecheck_Exprs.cpp's NewExpr
// `out owned var` block, ~line 5162, landed by #2409) registers the inline
// recipient's LocalSymbol, but `ensureCtorOutVarSlots`
// (src/codegen/LlvmBackend_Calls2.cpp) — the codegen function that
// pre-creates the alloca slot `buildOrderedArgs` later expects to find via
// `locals_` — handled ONLY the literal `"out var"` modifier. An
// `"out owned var"` argument fell through untouched, so `buildOrderedArgs`
// found no registered local for the fresh identifier when resolving the
// by-ref argument address (`isByRefMod` already lists `"out owned var"`,
// so it routes through `lvalueAddr`) and hit the generic #1386 ICE guard:
// "internal: lowerExpr returned null for call argument in buildOrderedArgs
// — #1386" (confirmed reproducing byte-for-byte, same column, on stock
// main via `git apply -R` + rebuild, before this fix).
//
// FIX #1: `ensureCtorOutVarSlots` gained an `"out owned var"` branch
// mirroring `ensureOutVarSlots`' identical CallExpr block — an owned
// recipient is always OBJECT-typed (§22.4's `out owned` is only ever
// admitted on a class/interface parameter), so the slot is a bare
// null-initialized pointer alloca, recorded in `locals_` and
// `outOwnedRecipients_`, with the same normal-exit/exception-path
// `unwindOnly`/`dynamicDispose` disposal registration as the method-call
// sibling.
//
// ROOT CAUSE #2 (found during this fix's own adversarial self-pass — a
// throw-after-assign leak the ICE previously made unreachable):
// `UnwindIneligibleWalker`'s NewExpr argument walk (LlvmBackend_Setup.cpp)
// is a hand-written loop that, unlike `walkCallArgs` (the CallExpr/
// NullCondCallExpr sibling), never exempted `out`/`out var`/`out using var`/
// `out owned`/`out owned var` arguments via `isOutArg` — every constructor
// out-argument was unconditionally treated as CONSUMING, wrongly tainting
// the inline `out owned var` recipient as unwind-INELIGIBLE. That taint
// suppressed `ensureCtorOutVarSlots`' own `unwindOnly` disposal
// registration (its `!unwindIneligible_.count(declKey)` guard), so a
// constructor that assigned the out-owned parameter and then THREW leaked
// the just-written value — confirmed live via LSan (16-byte Direct leak
// from inside the generated constructor) before this half of the fix.
//
// FIX #2: the NewExpr arm now checks `isOutArg(a.modifier)` FIRST, exactly
// as `walkCallArgs` does, and treats such an argument as non-consuming
// (`expr(a.value.get(), /*consuming=*/false)`) before falling through to
// the pre-existing explicit-`owned`-transfer untaint logic.
//
// KNOWN, FILED, NOT FIXED HERE (adversarial self-pass; see #2429): a
// constructor's `out owned var` recipient that is simply DROPPED ON THE
// FLOOR (never explicitly `.Dispose()`d, no throw involved) still LEAKS —
// confirmed live via LSan, no diagnostic emitted at all. Root cause is a
// THIRD, deeper instance of this issue's own recurring pattern: `recordMovesIn`'s
// NewExpr branch (src/ownership/Ownership_Checks.cpp) never calls
// `markOutOwnedArgs` (only `recordMovesInArgs`, the CallExpr path, does), so
// a constructor out-owned recipient's LocalSymbol never enters the
// ownership dataflow's `states_` map as Owned, and the end-of-scope affine
// auto-drop check (Ownership.cpp ~line 634-676) never sees it as a
// candidate for `autoDropAtScopeEnd` — the codegen registration this fix
// adds then falls back to `unwindOnly` (correct for the throw edge, but
// inert on normal fall-off). This is a genuinely separate subsystem
// (ownership-dataflow state, not codegen slot creation) and a materially
// larger/riskier change (interacting with `everOwnedPlainLocals_`,
// `preDeclaredOutOwnedRecipients_`, `loopCondOutOwnedSyms_`, and the
// `markOutOwnedArgs` builtin-vs-user-formal disambiguation) than this
// issue's own codegen-only scope — filed as #2429 rather than folded in
// here, per this project's small-PR / decline-with-reasons discipline. It
// is a bounded 16-byte-class leak (no double-free / no UAF; LSan-confirmed
// clean apart from the leak itself), not a soundness hazard, so no test
// below exercises that shape (a committed test must fail-without/pass-with
// THIS fix, and that scenario does not pass even with this fix).
//
// Tests (each uses runAotChecked → generated code linked + run under
// ASan/LSan, so a leak, double-free, or use-after-free FAILS via
// r.ran=false):
//   ice_fix_basic_construct_and_explicit_dispose_spec_22_4 — the issue's
//     own repro shape: compiles (no ICE) and disposes both the recipient
//     and the constructed instance exactly once.
//   throw_after_assign_disposes_on_unwind_no_leak_spec_22_3 — the
//     adversarial throw-partway case (root cause #2's regression): the
//     constructor assigns the out-owned recipient then throws; per §22.3
//     ("ownership reaches the caller only on normal return"), the value
//     must be disposed on the unwind edge with no leak and no double-free.
//   multiple_out_owned_params_independent_slots_no_leak_spec_22_4 — two
//     `out owned var` recipients in one constructor call get INDEPENDENT
//     slots (a regression guard against a positional-indexing mixup) and
//     both dispose exactly once.
//   using_var_receiver_with_out_owned_var_recipient_spec_28_10 — the
//     constructed instance is bound `using var` (scope-bound disposal)
//     while its `out owned var` recipient is disposed explicitly — both
//     discharge exactly once, no interaction/double-free between the two
//     disposal mechanisms.
//
// per §22.3, §22.4, §28.3, §28.10, #2409, #1386

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is
// absent: runAotChecked returns ran=false with a "SKIP" diagnostic — guard
// so an environment gap does not masquerade as this fix's own regression
// (mirrors #1934 / the #1833 test's identical guard).
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// Res — owned instance, destructor decrements a static counter read after
// disposal: Res.alive == 0 at the end proves exactly-once disposal of every
// constructor-minted Res (not merely "no crash") on top of ASan/LSan's own
// double-free / leak detection.
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
// FIX #1 — the issue's own repro: `new Widget(s, out owned var n)` must
// compile (no #1386 ICE) and run cleanly, disposing `n` and `w` exactly
// once each. Pre-fix this failed to compile at all.
// ===========================================================================
BEZEL_TEST(ctor_out_owned_var_slot_2411,
           ice_fix_basic_construct_and_explicit_dispose_spec_22_4) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Widget "
        "  public constructor(s: string, out owned n: Res) "
        "    n := new Res(9); "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    var w := new Widget(\"x\", out var n); "
        "    var idOk := (n.Id = 9); "
        "    n.Dispose(); "
        "    w.Dispose(); "
        "    if idOk then return Res.alive; end; "
        "    return -1; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: does not even compile (#1386 ICE)
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // Res.alive settles at 0: +1 mint, -1 dispose
}

// ===========================================================================
// FIX #2 — the adversarial throw-partway regression: the constructor
// ASSIGNS the out-owned recipient, then THROWS. Per §22.3 ("a value
// assigned to it before a later throw is discharged by the callee's
// ordinary exception-path discharge ... ownership reaches the caller only
// on normal return"), the just-written Res must be disposed on the unwind
// edge with no leak. Pre-fix-#2 (with fix #1 alone applied) this leaked 16
// bytes from inside the generated constructor (LSan Direct leak,
// confirmed live) — root cause #2 in this file's header comment.
// ===========================================================================
BEZEL_TEST(ctor_out_owned_var_slot_2411,
           throw_after_assign_disposes_on_unwind_no_leak_spec_22_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Boom: Exception "
        "  public constructor() base(\"boom\"); end constructor; "
        "end Boom; "
        "class Widget "
        "  public constructor(s: string, out owned n: Res) "
        "    n := new Res(1); "
        "    throw new Boom(); "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    try "
        "      var w := new Widget(\"x\", out var n); "
        "      return -1; "                 // unreachable; ctor throws first
        "    catch Boom => null; end; "
        "    return Res.alive; "             // 0 -> +1 mint, -1 unwind-dispose
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    // pre-fix-#2: LSan Direct leak flags ran=false (the mint is never
    // disposed on the unwind edge — see this file's header, root cause #2).
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// Two `out owned var` recipients in ONE constructor call must get
// INDEPENDENT slots — a regression guard on `ensureCtorOutVarSlots`'
// positional `pos` bookkeeping (a copy-paste/off-by-one there could alias
// both recipients onto the same alloca). Distinct `Id`s prove the slots are
// genuinely separate; both dispose exactly once (Res.alive settles at 0).
// ===========================================================================
BEZEL_TEST(ctor_out_owned_var_slot_2411,
           multiple_out_owned_params_independent_slots_no_leak_spec_22_4) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Widget "
        "  public constructor(out owned a: Res, out owned b: Res) "
        "    a := new Res(1); "
        "    b := new Res(2); "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    var w := new Widget(out var x, out var y); "
        "    if x.Id <> 1 then return -1; end; "
        "    if y.Id <> 2 then return -2; end; "
        "    x.Dispose(); "
        "    y.Dispose(); "
        "    w.Dispose(); "
        "    return Res.alive; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// The constructed instance bound `using var` (§28.10 scope-bound disposal)
// alongside its own `out owned var` recipient, disposed explicitly: the two
// independent disposal mechanisms (using-var scope-end vs. explicit
// .Dispose()) must not interact/double-free each other. Res.alive settles
// at 0 (Widget itself carries no Res, so this exercises only the recipient
// side, but through the using-var-bound constructor call shape).
// ===========================================================================
BEZEL_TEST(ctor_out_owned_var_slot_2411,
           using_var_receiver_with_out_owned_var_recipient_spec_28_10) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Widget "
        "  public constructor(s: string, out owned n: Res) "
        "    n := new Res(4); "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    using var w := new Widget(\"x\", out var n); "
        "    n.Dispose(); "
        "    return Res.alive; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
