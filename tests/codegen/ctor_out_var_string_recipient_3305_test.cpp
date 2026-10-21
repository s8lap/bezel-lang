// Spec-derived regression tests for issue #3305 — sibling of #2641: a PLAIN
// inline `out var` recipient of `string` type at a CONSTRUCTOR call site
// (`new T(out var n: string)`) was never released at scope exit, leaking one
// heap string per call (39 B in 2 allocations, measured under LSan).
//
// WHAT THE DEFECT WAS. `ensureCtorOutVarSlots`
// (`src/codegen/LlvmBackend_Calls2.cpp`) creates the caller-side slot for each
// inline out-var argument at a constructor call site. Its `out owned var` and
// `out using var` arms each register a scope-end `disposeStack_` entry for the
// recipient; the PLAIN `out var` arm fell through to `locals_[id] = slot;`
// and `registerCtorOutVarStringRelease` was not invoked, so the recipient had
// no discharge on any path. The METHOD-call form was fixed at #2641; the
// constructor form was explicitly left out of #2641 ("the callee is a
// `ConstructorSymbol` — gate 1 would have to widen and be re-verified"), and
// this file is the regression net for that re-verification.
//
// WHY THAT IS A LEAK OF A REFERENCE THE CALLER OWNS. §22.3 "Parameter
// modifiers" (Bezel_Language_Spec.md v0.268) specifies `out` as
// "copy-out-on-normal-return: within the callee the parameter denotes a
// callee-local slot, freely readable after assignment; on normal return the
// slot's value is copied to the caller's recipient", and blesses this backend's
// implementation of it: "An implementation may pass a hidden reference". The
// callee's `n := <expr>` therefore stores through the CALLER's slot, and that
// store goes through `lowerStringRefParamStore` (#2321,
// `LlvmBackend_Setup.cpp`), which emits the §28.9 balance:
//
//     load-old → retain-new (or TRANSFER a +1 producer) → store-new → release-old
//
// So on return the caller's slot holds exactly ONE reference that no other party
// holds — a +1 minted or retained specifically for it. §28.9 "Runtime-managed
// types" puts `string` among the types whose "lifetime is managed by the runtime
// without user involvement", so reclaiming that reference is the
// IMPLEMENTATION's obligation, not the program's; this backend discharges it by
// refcount, and the scope-end release is the discharge every other string local
// already gets (`Disposal::stringRelease`, #422). Without it the reference is
// simply never decremented.
//
// WHY THIS FIX IS NOT A COPY OF THE METHOD VARIANT. The five gate predicates
// are the same — the callee is a `sema::ConstructorSymbol` rather than a
// `sema::MethodSymbol`, and the dual-sourcing story is `ctorDecl->params[pos]`
// (AST) + `ctorSym->params[pos]` (imported `ParameterSymbol`) rather than
// `md->params[pos]` + `ms->params[pos]`. The predicate is unchanged: a `string`
// non-nullable `out` formal still has the same reflex — the constructor body
// writes through the caller's slot via the same `lowerStringRefParamStore` —
// so the same release, with the same null-slot-after-release load-bearing
// behavior (the slot is entry-block hoisted, so a loop iteration N+1's callee
// load must observe the previous iteration's release, not freed memory).
//
// The EXCEPTION edge is not this fix's business and is not tested here: §22.3
// continues "on an exception exit the recipient is untouched — an inline
// `out var` recipient stays unassigned", so the slot still holds the null
// `ensureCtorOutVarSlots` stored and the release is a no-op on that edge.
//
// ⚠ THE ORACLE HERE IS LEAKSANITIZER, NOT THE EXIT CODE. Every test that pins
// the leak uses `runAotChecked` (generated-code ASan + LSan). On the PLAIN
// build these programs return the right value both before and after the fix —
// the defect is invisible to a value oracle. Conversely the issue's own
// repro returns the right value AND leaks 39 B under ASan (the very signal
// that drives this fix). Neither oracle dominates (#2633); this file needs
// the LSan one.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

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

}  // namespace

// -----------------------------------------------------------------------------
// #3305's own repro shape: one `new T(...)` call, one heap string copied out,
// the constructor's caller scope ends. The constructor's `s + "-tail"` is a
// +1 producer whose reference #2321 TRANSFERS into the caller's slot, so
// after the call the caller's slot is that string's only owner. Pre-fix
// nothing released it → LSan reports the leak → -999. Post-fix the
// scope-end release balances it and Main returns 6.
//
// Mirrors `single_call_out_var_string_recipient_is_leak_free` from #2641's
// regression net, but uses a constructor callee (the `ConstructorSymbol`
// path, gate 1's "widen" that #2641 explicitly left for this follow-up).
// per §22.4, §22.3, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(ctor_out_var_string_recipient_3305,
           single_call_ctor_out_var_string_recipient_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string) "
        "    n := s + \"-tail\"; ok := true; "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget(\"x\", out var n); "
        "    _ := w; "
        "    return n.Length; "
        "  end Main; end C;"), 6);
}

// -----------------------------------------------------------------------------
// #2641's LOOP program verbatim, but with the constructor call shape. The
// recipient's alloca is entry-block hoisted, so iteration N+1's callee
// store LOADS the slot to release what it displaces. If the scope-end
// release did not also null the slot, that load would see the pointer this
// iteration just freed — a double release, which ASan reports rather than
// tolerating. So this test fails BOTH ways: leak without the release,
// double-free without the null. The 5-iteration loop is also the exact
// program shape that produced the original 39 B / 2-allocations measurement
// at the METHOD call site under #2641.
// per §22.4, §22.3, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(ctor_out_var_string_recipient_3305,
           loop_reused_ctor_out_var_slot_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string) "
        "    n := s + \"-tail\"; ok := true; "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 5 do "
        "      var w := new Widget(\"x\", out var n); "
        "      _ := w; "
        "      total := total + n.Length; "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; end C;"), 30);
}

// -----------------------------------------------------------------------------
// The constructor assigns a BORROW (its own `string` parameter), not a
// producer. #2321 RETAINS in that case rather than transferring, so the
// caller's slot again owns exactly one independent reference — and the
// argument's own reference is still the caller's literal, untouched. A
// release that assumed "producer only" would under-release here (leak) and
// one that released the ARGUMENT would over-release it; both are caught.
// per §22.3, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(ctor_out_var_string_recipient_3305,
           borrow_assigned_ctor_out_var_recipient_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string) "
        "    n := s; ok := true; "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var src := \"ab\" + \"cde\"; "
        "    var w := new Widget(src, out var n); "
        "    _ := w; "
        "    return n.Length + src.Length; "
        "  end Main; end C;"), 10);
}

// -----------------------------------------------------------------------------
// The constructor assigns TWICE. The second store's release-of-old reclaims
// the first value, so the caller's slot still owns exactly one reference at
// return — the scope-end release must fire once, not once per assignment.
// per §22.3, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(ctor_out_var_string_recipient_3305,
           twice_assigned_ctor_out_var_recipient_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string) "
        "    n := s + \"-one\"; "
        "    n := s + \"-second\"; "
        "    ok := true; "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget(\"x\", out var n); "
        "    _ := w; "
        "    return n.Length; "
        "  end Main; end C;"), 8);
}

// -----------------------------------------------------------------------------
// TWO inline out-var string recipients in ONE constructor call. Each gets
// its own slot and its own registration; a fix that keyed the disposal on
// the call rather than on the argument would release one twice and the
// other never.
// per §22.4, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(ctor_out_var_string_recipient_3305,
           two_ctor_out_var_recipients_in_one_call_are_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out a: string, out b: string) "
        "    a := s + \"-a\"; b := s + \"-bb\"; ok := true; "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget(\"x\", out var a, out var b); "
        "    _ := w; "
        "    return a.Length + b.Length; "
        "  end Main; end C;"), 7);
}

// -----------------------------------------------------------------------------
// ESCAPE GUARD — the recipient is RETURNED out of the declaring method. The
// scope-end release must NOT fire: the caller of `Wrap` reads the string
// after `Wrap`'s scope has ended, so a scope-end release there would hand
// back freed memory. The fix's escape walk excludes the recipient, leaving
// the pre-existing leak. Run on the PLAIN oracle: the leak is real and
// deliberate (leak-over-UAF), so the LSan oracle would fail on the very
// behavior this test is asserting. What it pins is that the VALUE
// survives — with the escape gate removed, this reads a released string.
// per §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(ctor_out_var_string_recipient_3305,
           returned_ctor_out_var_recipient_is_not_released) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string) "
        "    n := s + \"-tail\"; ok := true; "
        "  end constructor; "
        "end Widget; "
        "class W "
        "  public static method Wrap(): string "
        "    var w := new Widget(\"x\", out var n); "
        "    _ := w; "
        "    return n; "
        "  end Wrap; "
        "end W; "
        "class C "
        "  public static method Main(): int "
        "    var got := W.Wrap(); "
        "    return got.Length; "
        "  end Main; end C;"), 6);
}

// -----------------------------------------------------------------------------
// SCOPE BOUNDARY — a NULLABLE `out n: string?` recipient. `lowerStringRefParamStore`
// declines a nullable target, so the callee's store leaves the caller's slot
// with no reference of its own; the fix mirrors that surface and registers
// nothing. This is a deliberate scope line (documented at
// `registerCtorOutVarStringRelease`), not an oversight: pinning it means a
// later widening of the fix must widen #2321 in the same commit or this
// test's plain-oracle value turns into a use-after-free. Plain oracle: the
// recipient's value, unchanged by this fix.
// per §22.3, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(ctor_out_var_string_recipient_3305,
           nullable_ctor_out_var_recipient_scope_boundary) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string?) "
        "    n := s + \"-tail\"; ok := true; "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget(\"x\", out var n); "
        "    _ := w; "
        "    if n <> null then return n.Length; end; "
        "    return 0; "
        "  end Main; end C;"), 6);
}

// -----------------------------------------------------------------------------
// CONTROL — a PRIMITIVE `out var` recipient carries no reference at all and
// must gain no disposal. Guards against a widening that registered a
// `bz_string_release` on an `int` slot (which would read the integer as a
// pointer). Value + LSan.
// per §22.4, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(ctor_out_var_string_recipient_3305,
           primitive_ctor_out_var_recipient_control_unaffected) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: int) "
        "    n := 7; ok := true; "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget(\"x\", out var n); "
        "    _ := w; "
        "    return n; "
        "  end Main; end C;"), 7);
}

// -----------------------------------------------------------------------------
// fix #3321 — a PRE-DECLARED (non-inline) `out n: string` recipient at a
// CONSTRUCTOR call site leaks at scope end unless the escape walk knows
// `out`-modified arguments at a non-nullable `string` formal are a WRITE
// into the slot, not an escape of the local. Same 39 B leak as #3305's
// inline form, different cause: #3305's `registerCtorOutVarStringRelease`
// arm structurally never runs for a pre-declared recipient (its slot
// already exists, and the call-site modifier is `out`, not `out var`), so
// the fix is in `StringEscapeWalker::walkNewArgs` instead — see that arm's
// `out`-modifier relaxation comment in `src/codegen/LlvmBackend_Setup.cpp`.
//
// ORACLE — LeakSanitizer (the leak is invisible on the plain build, exactly
// as #3305's leak was). Pre-fix: "39 byte(s) leaked in 2 allocation(s)"
// from `bz$M.Widget$ctor` (measured). Post-fix: zero leaks. Value (6) is
// pinned too to catch a fix that over-reached and registered a SECOND
// release on a slot whose own machinery may already own one (a double-free
// would crash under ASan).
//
// This test was previously the "control unaffected" placeholder for the
// #3305 fix (which deliberately did not cover the pre-declared shape); the
// issue #3321 body noted that it would be upgraded to `runChecked` once the
// separate defect landed. That landing is this lane. See
// `tests/codegen/predeclared_ctor_out_var_string_recipient_3321_test.cpp`
// for the comprehensive regression net (loop, borrow-assigned, twice-
// assigned, two-recipients, escape guards, primitive control).
// per §22.3, §22.7, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(ctor_out_var_string_recipient_3305,
           predeclared_ctor_out_recipient_control_unaffected) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string) "
        "    n := s + \"-tail\"; ok := true; "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var n: string := \"zz\"; "
        "    var w := new Widget(\"x\", out n); "
        "    _ := w; "
        "    return n.Length; "
        "  end Main; end C;"), 6);
}

// -----------------------------------------------------------------------------
// CONTROL — the existing ctor-out-slot mechanism for `out owned var`
// (carried by the `out owned var` / `out using var` arms of
// `ensureCtorOutVarSlots`, #2411 / #2430) must keep working after this
// fix. The constructor here declares an `out owned` parameter (a class-
// typed owned slot, per §28.1 — `out owned` on a value type is rejected),
// and the call site uses the inline `out owned var` form. The Res.alive
// counter settles at 0 after the run, proving the recipient is
// disposed exactly once (no leak, no double-free) — the same verification
// the #2411/#2430 tests perform for the `out owned var` / `out using var`
// pair. Pinning it here catches any future regression where the new
// `registerCtorOutVarStringRelease` path INTERFERES with the existing
// `out owned var` arm (e.g. a slot-bookkeeping conflict in
// `ensureCtorOutVarSlots`).
// per §22.3, §22.4, §28.3
// -----------------------------------------------------------------------------
BEZEL_TEST(ctor_out_var_string_recipient_3305,
           out_owned_var_ctor_existing_mechanism_still_works) {
    JitResult r = runAotChecked(
        "module M; class Res "
        "  public static alive: int := 0; "
        "  public Id: int; "
        "  public constructor(id: int) Id := id; Res.alive := Res.alive + 1; "
        "    end constructor; "
        "  destructor Res.alive := Res.alive - 1; end destructor; "
        "end Res; "
        "class Widget "
        "  public ok: bool; "
        "  public constructor(out owned w: Res) "
        "    w := new Res(9); ok := true; "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    var w := new Widget(out var n); "
        "    _ := w; "
        "    var idOk := (n.Id = 9); "
        "    if idOk then return 0; end; "
        "    return -1; "
        "  end Main; "
        "end C;");
    if (!r.ran) {
        // #3733: a toolchain SKIP fails this row (support/aot_result_asserts.hpp).
        BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
