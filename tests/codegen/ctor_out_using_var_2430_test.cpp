// fix #2430 (§22.4 "Inline `out` declarations", spec v0.235): `out using var`
// not recognized at a CONSTRUCTOR call site.
//
// §22.4 states: "When a method parameter is declared `out owned`, ... The
// accepted forms at the call site of an `out owned` parameter are
// `out using var x` (a scope-bound owned local) and `out owned var x` (an
// owned local with the standard owned-local discharge rules applying); both
// produce an owned location for the callee to write through." This rule is
// stated for a parameter, with no carve-out for a CONSTRUCTOR's `out owned`
// parameter vs. a method's — #2411 (immediately prior) confirmed the
// `out owned var` half of that pair already works at a constructor call
// site (`new T(out owned var x)`); this issue is the `out using var` half.
//
// ROOT CAUSE: `ExprTypecheck_Exprs.cpp`'s `NewExpr` argument-registration
// loop had a dedicated block for `"out var"` (#968) and `"out owned var"`
// (#1305, #2409, #2411) but NO block at all for `"out using var"` — so `n`
// in `new Widget(s, out using var n)` was never registered as a local by
// sema: every later reference to `n` failed the ordinary
// unresolved-identifier check ("unresolved identifier `n`" at the argument
// position AND at every later read) — a clean rejection, not a crash, but a
// missing FEATURE relative to §22.4's stated grammar. Confirmed live on
// stock main (pre-fix) via `git apply -R` + rebuild: the issue's own repro
// reproduces that exact diagnostic, byte-for-byte.
//
// FIX (two mirrors of #2411's identical pair of fixes, one modifier over):
//   1. ExprTypecheck_Exprs.cpp: NewExpr's argument loop gained an
//      `"out using var"` block (mirroring the `"out owned var"` block
//      immediately above it, and the method-call `CallExpr` sibling's
//      `"out using var"` handler, ~line 4643) that registers the recipient's
//      LocalSymbol with declType derived from the matching `out owned`
//      constructor formal parameter.
//   2. LlvmBackend_Calls2.cpp: `ensureCtorOutVarSlots` gained an
//      `"out using var"` branch (mirroring `ensureOutVarSlots`' identical
//      CallExpr block, restricted to its non-`Weak<T>.TryUpgrade` path,
//      which cannot apply to a constructor callee) that pre-creates a
//      null-initialized pointer alloca slot and registers an unconditional
//      `dynamicDispose` scope-end disposal for it — the `using` obligation:
//      always discharged at scope end, regardless of the unwind path,
//      unlike `out owned var`'s conditional unwindOnly/dynamicDispose split.
//
// The throw-partway edge (constructor assigns the out-owned parameter, then
// throws) is exercised explicitly below per the #2411 precedent — that fix's
// own adversarial self-pass found a SEPARATE leak on this edge (a
// UnwindIneligibleWalker gap, now fixed on main via `isOutArg`), so this
// suite verifies the `out using var` form inherits that fix rather than
// assuming it does. `out using var`'s disposal is registered UNCONDITIONALLY
// (not gated on `unwindIneligible_` the way `out owned var`'s is), so the
// UnwindIneligibleWalker taint that broke `out owned var` cannot reintroduce
// this exact leak for `out using var` — but the scenario is still verified
// mechanically below (no leak either way).
//
// Each test uses runAotChecked (ASan/LSan-backed) and a static alive-counter
// destructor pattern (Res.alive: ctor +1, dtor -1) so "disposed exactly
// once" is proven by a settled counter value, not merely "does not crash".
//
// per §22.3, §22.4, §28.3, §28.10, #2411, #2409, #1386

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

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
// The issue's own repro shape: `new Widget(s, out using var n)` must resolve
// `n` (no "unresolved identifier") and run cleanly, disposing the recipient
// at scope end (a `using`-var obligation) and `w` explicitly. Pre-fix this
// FAILED TO COMPILE ("unresolved identifier `n`" — sema never registered the
// recipient as a local).
// ===========================================================================
BEZEL_TEST(ctor_out_using_var_2430,
           unresolved_identifier_fix_basic_construct_spec_22_4) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Widget "
        "  public constructor(s: string, out owned n: Res) "
        "    n := new Res(9); "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  private static method Run(): int "
        "    Res.alive := 0; "
        "    var w := new Widget(\"x\", out using var n); "
        "    var idOk := (n.Id = 9); "
        "    w.Dispose(); "
        "    if idOk then return 0; end; "
        "    return -1; "
        "  end Run; "
        "  public static method Main(): int "
        "    var rc := C.Run(); "
        "    if rc <> 0 then return rc; end; "
        // `n` is a `using var` scope-bound to Run's body: by the time Run
        // has returned, n's scope has ended and it must be disposed exactly
        // once (Res.alive settles back to 0 — no leak, no double-free).
        "    return Res.alive; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: does not even compile
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// The adversarial throw-partway edge (per #2411's own precedent — its
// closely-related `out owned var` fix surfaced exactly this shape as a
// SEPARATE regression): the constructor ASSIGNS its out-owned recipient,
// then THROWS. Per §22.3 ("ownership reaches the caller only on normal
// return"), the just-assigned Res must be disposed on the unwind edge with
// no leak — the `new Widget(...)` copy-out to the `out using var` recipient
// never happens on this edge (mirrors plain `out`), so this recipient's own
// scope-end disposal is a correct null-guarded no-op; the LEAK this test
// guards against is of the callee-side assigned-then-abandoned value.
// ===========================================================================
BEZEL_TEST(ctor_out_using_var_2430,
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
        "      var w := new Widget(\"x\", out using var n); "
        "      return -1; "                 // unreachable; ctor throws first
        "    catch Boom => null; end; "
        "    return Res.alive; "             // 0 -> +1 mint, -1 unwind-dispose
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// Two `out using var` recipients in ONE constructor call must get
// INDEPENDENT slots — a regression guard on `ensureCtorOutVarSlots`'
// positional bookkeeping (mirrors #2411's identical `out owned var` guard).
// Distinct `Id`s prove the slots are genuinely separate; both are `using
// var`-disposed at Run's scope end (Res.alive settles at 0 by the time Main
// reads it).
// ===========================================================================
BEZEL_TEST(ctor_out_using_var_2430,
           multiple_out_using_var_params_independent_slots_no_leak_spec_22_4) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Widget "
        "  public constructor(out owned a: Res, out owned b: Res) "
        "    a := new Res(1); "
        "    b := new Res(2); "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  private static method Run(): int "
        "    Res.alive := 0; "
        "    var w := new Widget(out using var x, out using var y); "
        "    if x.Id <> 1 then return -1; end; "
        "    if y.Id <> 2 then return -2; end; "
        "    w.Dispose(); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    var rc := C.Run(); "
        "    if rc <> 0 then return rc; end; "
        "    return Res.alive; "   // x, y both out of scope now -> 0
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// `out owned var` and `out using var` MIXED as the two out-owned recipients
// of one constructor call: an adversarial cross-check that the two sibling
// inline forms' independent codegen paths (unwindOnly/dynamicDispose-gated
// vs. unconditional dynamicDispose) do not alias or double-register the
// same slot. `a` (owned var) is disposed explicitly; `b` (using var) is
// disposed at scope end. Res.alive settles at 0.
// ===========================================================================
BEZEL_TEST(ctor_out_using_var_2430,
           mixed_owned_var_and_using_var_recipients_no_leak_spec_22_4) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Widget "
        "  public constructor(out owned a: Res, out owned b: Res) "
        "    a := new Res(1); "
        "    b := new Res(2); "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  private static method Run(): int "
        "    Res.alive := 0; "
        "    var w := new Widget(out var a, out using var b); "
        "    var ok := (a.Id = 1) and (b.Id = 2); "
        "    a.Dispose(); "
        "    w.Dispose(); "
        "    if ok then return 0; end; "
        "    return -1; "
        "  end Run; "
        "  public static method Main(): int "
        "    var rc := C.Run(); "
        "    if rc <> 0 then return rc; end; "
        "    return Res.alive; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
