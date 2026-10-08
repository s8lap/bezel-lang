// Spec-derived regression tests for issue #5672 — the CONSTRUCTOR twin of the
// #3842 fix-forward's method-call block. `out var x` at a constructor's
// `out owned` formal (`new Widget(out var x)`) is an OWNED LOCAL under
// §22.4, but `ensureCtorOutVarSlots`
// (src/codegen/LlvmBackend_Calls2.cpp) registered neither half the method arm
// registers: no `outOwnedRecipients_.insert` and no scope-end disposal.
//
// THE TWO HALVES, TWO LEAKS, BOTH MEASURED (50 iterations, ASan-armed, LSan):
//
//   explicit `x?.Dispose()`:  816 B in 51 objects — the destructor ran but the
//     heap block was never reclaimed, because without the
//     `outOwnedRecipients_.insert` an explicit `.Dispose()` on this recipient
//     only DISPATCHES the destructor (the §28.11/#889 reclaim path keys on
//     that set) instead of reclaiming-and-nulling the slot.
//   undischarged (must-use warning only): 816 B in 51 objects — nothing
//     disposed at all on the normal fall-off edge.
//   The method-call twin of each program compiled and ran clean (EXIT=0), so
//   the asymmetry was the constructor arm's alone. LSan named the generated
//   `bz$M.Widget$ctor` as the allocating frame: the callee minted the object
//   and nothing on the caller's side freed it.
//
// The ownership PASS already honoured this shape — its NewExpr arm calls
// `markOutOwnedArgs(ne->args, ctorParams)`
// (src/ownership/Ownership_Checks.cpp), which is what makes
// `autoDropAtScopeEnd` come out true and what emits the §28.3 must-use
// warning at the `x` column (asserted below). Codegen was the half that never
// followed, so the obligation the pass recorded was dropped on the floor.
//
// ⚠ THE ORACLE IS LEAKSANITIZER, NOT THE EXIT CODE. On the plain build these
// programs return the right value both before and after the fix; the defect is
// invisible to a value oracle. Every row here uses `runAotChecked`
// (generated-code ASan + LSan) so a leak / double-free / UAF surfaces as
// ran=false → the -999 sentinel.
//
// ⚠ THE SECOND DIRECTION IS THE ONE A TOO-WIDE GATE BREAKS. An undischarged
// recipient is the shape whose fix is "register a scope-end disposal"; the
// EXPLICITLY discharged shape is the shape that fix can double-free. Both are
// pinned below, per §28.3/§28.11's exactly-once rule: the disposal pair is
// `dynamicDispose` when `autoDropAtScopeEnd` is set and `unwindOnly`
// otherwise, both null-guarded, so an explicit discharge nulls the slot and
// the later scope-end pass finds null.
//
// per §22.4 (v0.315), §28.3, §28.11, §28.1 (v0.196, #1860), §22.3

#include <string>

#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "support/ownership_support.hpp"   // parseAndCheckOwnership
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::JitResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is
// absent: runAotChecked returns ran=false with a "SKIP" diagnostic. Guard so
// an environment gap does not masquerade as this fix's own regression
// (mirrors #1934 / the #2411 test's identical guard).
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// ASan + LSan oracle: a leak / double-free / UAF report at exit surfaces as
// ran=false, which this flips to the -999 sentinel.
int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// Plain (non-sanitized) oracle — value only.
int run(const std::string& src) {
    JitResult r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// ⚠ runAot / runAotChecked's `JitResult.diagnostics` carries only synthetic
// infra-failure text on a SUCCESSFUL compile — a successful compile's own
// warnings are discarded, never attached (see aot_support.hpp: it calls
// r.diagnostics.report(...) only on an infra failure or a non-Ok outcome).
// So a warning's PRESENCE cannot be read off a `runAot` result at all.
// `parseAndCheckOwnership` drives lex+parse+sema+ownership directly and
// keeps every diagnostic, warnings included.
bool autoDropWarns(const std::string& src, const std::string& name) {
    auto r = parseAndCheckOwnership(src);
    return hasAutoDropWarning(r, name);
}

// Res — a disposable class whose destructor decrements a static counter.
// `Res.alive == 0` at the end proves every constructor-minted Res was
// disposed EXACTLY once on top of ASan/LSan's own leak / double-free
// detection (a destructor that ran twice would drive it negative).
//
// ⚠ `alive` is an `int` static and therefore carries the §22.12 rule-2
// declaration-site warning on every program below. That is expected and is
// not asserted against anywhere in this file.
const char* kRes =
    "class Res: IDisposable "
    "  public static alive: int := 0; "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; Res.alive := Res.alive + 1; "
    "    end constructor; "
    "  destructor Res.alive := Res.alive - 1; end destructor; "
    "end Res; ";

// The issue's own repro shape: a constructor taking an `out owned` formal
// and minting into it. `R` keeps a BORROW so the recipient — not the
// constructed object — is the sole owner, which is exactly the shape whose
// block was leaking.
const char* kWidget =
    "class Widget "
    "  public R: Res?; "
    "  public constructor(out owned r: Res?) "
    "    r := new Res(7); "
    "    R := r; "
    "  end constructor; "
    "end Widget; ";

}  // namespace

// ⚠ EVERY leak repro here LOOPS 50 times. A one-shot `new` leaks ~16 B and
// LSan can miss it; the loop is what makes the pre-fix signal unambiguous
// (816 B in 51 objects, measured).
//
// BEZEL loop syntax is `for var i in A..B do ... end for;`.

// ===========================================================================
// DIRECTION 1 — explicitly discharged. `x?.Dispose()` must RECLAIM the heap
// block (and null the slot), not merely dispatch the destructor. Pre-fix:
// 816 B in 51 objects leaked even though every destructor ran.
// ===========================================================================
BEZEL_TEST(ctor_out_var_out_owned_recipient_5672,
           explicit_dispose_reclaims_recipient_no_leak_spec_22_4) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kRes + kWidget +
        "class W "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    for var i in 0..50 do "
        "      var w := new Widget(out var x); "
        "      x?.Dispose(); "
        "    end for; "
        "    return Res.alive; "
        "  end Main; "
        "end W;"), 0);
}

// ===========================================================================
// DIRECTION 2 — UNdischarged. The §28.3 must-use warning is the only
// treatment; the scope-end affine auto-drop must fire. Pre-fix: 816 B in 51
// objects, nothing disposed at all. This is the row the `dynamicDispose`
// (vs `unwindOnly`) choice is pinned by.
// ===========================================================================
BEZEL_TEST(ctor_out_var_out_owned_recipient_5672,
           undischarged_recipient_autodrops_at_scope_end_spec_28_3) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kRes + kWidget +
        "class W "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    for var i in 0..50 do "
        "      var w := new Widget(out var x); "
        "      _ := w; "
        "    end for; "
        "    return Res.alive; "
        "  end Main; "
        "end W;"), 0);
}

// ===========================================================================
// DIRECTION 2's MUST-USE WARNING. §22.4: an `out var x` at an `out owned`
// formal "is an owned local under the standard owned-local discharge rules,
// the must-use warning included". The ownership pass emits this on the
// CONSTRUCTOR path (its NewExpr arm calls markOutOwnedArgs); codegen is what
// was missing. Pinned so a future change that drops the recipient's Owned
// classification cannot quietly also drop the auto-drop this file's row 2
// depends on.
//
// ⚠ Read through `parseAndCheckOwnership`, NOT `runAot`: a successful
// compile's own warnings never reach `JitResult.diagnostics`, so a runAot-based
// assertion here would be VACUOUS (it would read zero diagnostics before the
// fix and zero after, passing either way).
// ===========================================================================
BEZEL_TEST(ctor_out_var_out_owned_recipient_5672,
           undischarged_recipient_warns_must_use_spec_28_3) {
    BEZEL_EXPECT_TRUE(autoDropWarns(
        std::string("module M; ") + kRes + kWidget +
        "class W "
        "  public static method Main(): int "
        "    var w := new Widget(out var x); "
        "    _ := w; "
        "    return 0; "
        "  end Main; "
        "end W;", "x"));
}

// ===========================================================================
// EXACTLY-ONCE, the direction a too-wide gate double-frees. Disposing the
// recipient EXPLICITLY and then letting the scope-end disposal run must free
// the block ONCE. Without the null-on-disposal that the `outOwnedRecipients_`
// reclaim performs, the scope-end `dynamicDispose`/`unwindOnly` pass would
// dispose the same object again — ASan reports a double-free and the row
// fails. `Res.alive` returning 0 rather than -50 is the value-oracle half of
// the same fact: a destructor that ran twice per iteration drives it to -50.
//
// `x!` is the non-null form: §7.4 bars `.Dispose` on a nullable receiver, so
// the row would not even compile without the force-unwrap.
//
// ⚠ THIS ROW IS ALSO A FAIL-WITHOUT ROW, but for the LEAK reason, not the
// double-free one. Measured with this fix reverted: it FAILS (LSan, 816 B in
// 51 objects) because without the `outOwnedRecipients_.insert` the explicit
// `x!.Dispose()` dispatches the destructor without reclaiming — the same
// defect row 1 pins, reached through the non-null discharge spelling. Its
// distinct value is on the POST-fix side: it is the row that goes red if the
// scope-end disposal is ever registered too widely (no null-on-disposal ⇒
// double free ⇒ ASan aborts; `Res.alive` would read -50, not 0). The
// `move`-based sibling below is the second such net.
// ===========================================================================
BEZEL_TEST(ctor_out_var_out_owned_recipient_5672,
           explicit_dispose_then_scope_end_does_not_double_free_spec_28_3) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kRes + kWidget +
        "class W "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    for var i in 0..50 do "
        "      var w := new Widget(out var x); "
        "      if x <> null then x!.Dispose(); end if; "
        "      _ := w; "
        "    end for; "
        "    return Res.alive; "
        "  end Main; "
        "end W;"), 0);
}

// ===========================================================================
// THE SECOND EXACTLY-ONCE DISCHARGE SPELLING. §28.3's must-use message names
// `item?.Dispose()`, `_ := item`, `move`, or `return` as the four explicit
// discharges. `move` transfers the obligation to `y`, which then disposes it —
// so the pair (recipient `x` → `move` → `y` → `y?.Dispose()`) must free once
// across TWO scope-end registrations. A gate that registered the disposal
// twice, or registered it on `x` AND `y`, double-frees here.
// ===========================================================================
BEZEL_TEST(ctor_out_var_out_owned_recipient_5672,
           moved_recipient_discharged_at_new_owner_no_double_free_spec_28_3) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kRes + kWidget +
        "class W "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    for var i in 0..50 do "
        "      var w := new Widget(out var x); "
        "      var y := move x; "
        "      y?.Dispose(); "
        "      _ := w; "
        "    end for; "
        "    return Res.alive; "
        "  end Main; "
        "end W;"), 0);
}

// ===========================================================================
// OVER-FIRE GUARD, value substitution. §28.1 (v0.196, #1860): at a
// value/runtime-managed substitution the `out owned T` marker is ERASED — the
// recipient owns nothing, so the fix must register NOTHING there. `Box<int>`
// instantiates `out owned b: T?` at `int`: a scope-end `dynamicDispose` here
// would dispose an erased value as an object (a segfault). `Box<Res>`
// instantiates the SAME formal at a class and must still dispose.
//
// `Stack<int>.TryPop(out var item)` is the method-call twin of the same guard
// (§28.11/#3779) and is included so the two paths are pinned together: a fix
// that widened EITHER gate past its formal breaks this row.
// ===========================================================================
BEZEL_TEST(ctor_out_var_out_owned_recipient_5672,
           value_substitution_registers_nothing_spec_28_1) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kRes +
        "class Box<T> "
        "  public V: int; "
        "  public constructor(out owned b: T?) "
        "    b := null; "
        "    V := 7; "
        "  end constructor; "
        "end Box; "
        "class W "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    var b := new Box<int>(out var vi); "
        "    var b2 := new Box<Res>(out var cr); "
        "    cr?.Dispose(); "
        "    var st := new Stack<int>(); "
        "    st.Push(1); "
        "    var ok := st.TryPop(out var item); "
        "    _ := item; "
        "    if b.V = 7 and b2.V = 7 and Res.alive = 0 then return 0; end; "
        "    return -1; "
        "  end Main; "
        "end W;"), 0);
}

// ===========================================================================
// OVER-FIRE GUARD, borrowing formal. A plain `out` constructor formal is a
// BORROW: §22.4's adoption clause gives `out var x` at a plain `out` formal
// "a borrow … whose sources are the call's". The recipient must therefore
// register nothing — the CALLEE (or the caller) owns that string. If the fix
// keyed on the call-site `out var` modifier alone, this row would reclaim a
// reference it never owned (a double-release against `S`, which also retains
// it) or leak-free-run by luck.
// ===========================================================================
BEZEL_TEST(ctor_out_var_out_owned_recipient_5672,
           borrowing_out_formal_registers_nothing_spec_22_4) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Sink "
        "  public S: string; "
        "  public constructor(out s: string?) "
        "    s := \"borrowed-by-callee\"; "
        "    S := s; "
        "    s := null; "
        "  end constructor; "
        "end Sink; "
        "class W "
        "  public static method Main(): int "
        "    var k := new Sink(out var sv); "
        "    _ := sv; "
        "    if k.S = \"borrowed-by-callee\" then return 0; end; "
        "    return -1; "
        "  end Main; "
        "end W;"), 0);
}

// ===========================================================================
// VALUE ORACLE for direction 1 — the recipient's field reads back correctly
// after the `new`, on the PLAIN (non-sanitized) path. runAotChecked fills
// diagnostics only on a reject, and the plain build cannot see a leak, so
// this row exists to keep the fix honest on the second oracle too: the
// recipient must be READABLE (a null-on-disposal that ran too early would
// leave the slot nulled before `x.Id`).
// ===========================================================================
BEZEL_TEST(ctor_out_var_out_owned_recipient_5672,
           recipient_readable_after_constructor_returns_spec_22_4) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kRes + kWidget +
        "class W "
        "  public static method Main(): int "
        "    var w := new Widget(out var x); "
        "    var idOk := (x?.Id = 7); "
        "    x?.Dispose(); "
        "    _ := w; "
        "    if idOk then return 0; end; "
        "    return -1; "
        "  end Main; "
        "end W;"), 0);
}