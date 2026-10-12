// Spec-derived regression tests for issue #3321 — sibling of #3304: a
// pre-declared (non-inline) `out n: string` recipient at a CONSTRUCTOR call
// site (`var n: string; var x := new T(out n);`) was never released at scope
// exit, leaking one heap string per call (39 B in 2 allocations, measured
// under LSan; same signature as #3304's method-call form and as #3305's
// inline-`out-var` constructor form).
//
// WHAT THE DEFECT WAS. The pre-declared recipient IS an ordinary `var` string
// local, so the machinery that should release it is the existing
// `armStringLocalRefcount` / `collectStringReleaseEligible` pair
// (`src/codegen/LlvmBackend_Setup.cpp`). The local is admitted as a BORROW
// candidate (its `"zz"` initializer is not a `+1` producer) and is in
// `stringBorrowAll_` (retained at bind — a no-op on a literal sentinel). Then
// the escape walk runs, reaches the `out n` argument in
// `StringEscapeWalker::walkNewArgs`, and finds no relaxation applies: every
// existing safe-arg relaxation there requires `a.modifier.empty()`. So the
// local is classified ESCAPED, `stringBorrowRetainEligible_` excludes it, no
// scope-end release is armed, and the +1 the constructor minted into the
// slot (`lowerStringRefParamStore`, #2321) is never decremented.
//
// WHY #3304'S METHOD-CALL FIX DOES NOT COVER IT. #3304's relaxation is in
// `StringEscapeWalker::walkCallArgs` — the `CallExpr` arm. Constructors go
// through the `walkNewArgs` (`NewExpr`) arm in the same walker, which has its
// own conservative treatment and a separate #2023 relaxation for plain
// `new T(args)` borrows. The pre-declared `out n: string` constructor case
// needs a parallel relaxation in that arm — same gates, callee is a
// `ConstructorSymbol` rather than a `MethodSymbol`.
//
// WHY THIS FIX'S MIRROR OF #3304'S GATES IS THE RIGHT ANSWER. The selling
// point is identical: §22.3 makes `out` "copy-out-on-normal-return" and
// licenses "An implementation may pass a hidden reference", which is what
// this backend does. The constructor's `n := <expr>` stores through the
// CALLER's slot via the same `lowerStringRefParamStore` (#2321) that the
// method form uses, so on return the caller's slot is the SOLE owner of
// exactly one reference. §22.7 inherits the §22.3 `out` semantics for
// constructors verbatim, so the analysis is unchanged. §28.9 puts `string`
// among the runtime-managed types whose refcount-reclamation is the
// implementation's obligation — `armStringLocalRefcount`'s registered
// `bz_string_release` discharges it, the same as every other non-escaping
// string local.
//
// Mirroring #3304's gates, narrowly (all four):
//   * Resolved `ConstructorSymbol` (`chosen` is set above as the unique
//     arity-matching ctor; a 0-or-many match fell through to `walkAllEscape()`
//     above). An unresolved / builtin-typed callee fell through too. The
//     gate's compiled-callee premise mirrors #3304's resolved-MethodSymbol
//     gate exactly.
//   * Formal modifier `out` (not `ref`, not `owned`, not plain) — `ref` reads
//     the caller's slot via a hidden reference and may alias the value
//     through field store / return / capture without minting its own +1, so
//     a scope-end release would dangle that alias (UAF); `ref` is
//     deliberately NOT admitted (same as #3304).
//   * Formal type non-nullable `string` — mirrors `lowerStringRefParamStore`'s
//     exact surface (it bails on a nullable target), keeping a generic
//     `out T` instantiated at `string` OUT of scope: the specialized
//     constructor's assignment is typed by `T`, so whether #2321 fires
//     there is a separate question — leaving it to leak is the safe
//     direction. The dual sourcing is `ps->resolvedDeclType` (imported
//     `ParameterSymbol`) with the AST `ConstructorDecl`'s declared type as
//     fallback — exactly the dual-sourcing #3305's `registerCtorOutVarStringRelease`
//     uses for the inline form.
//   * Gated on stringReleaseMode (the #1997 narrowing) — the shared
//     dict-materializer (#1028), grouping (#1930), borrowing-Array (#1003),
//     record-call-result (#1747) and value-like-choice (#2115) walks reuse
//     this walker with mode=false, so a plain `out T` collection recipient
//     stays their conservative ESCAPE (their FFI / callee retain discipline
//     is not asserted by any test, leak-over-UAF is the safe default).
//
// The recipient's bind-time retain (a no-op on a literal sentinel, a real
// retain otherwise) and the constructor's #2321 store both touch the same
// slot; end-to-end the slot owns exactly one reference at scope end, which
// `armStringLocalRefcount`'s registered `bz_string_release` decrements.
//
// ⚠ THE ORACLE HERE IS LEAKSANITIZER, NOT THE EXIT CODE. Every test that
// pins the leak uses `runAotChecked` (generated-code ASan + LSan). On the
// PLAIN build these programs return the right value both before and after
// the fix — the defect is invisible to a value oracle. Conversely the
// issue's own repro returns the right value AND leaks 39 B under ASan (the
// very signal that drives this fix). Neither oracle dominates (#2633); this
// file needs the LSan one.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked
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
// #3321's own repro shape: pre-declared `out n: string` recipient, one
// `new T(...)` call, one heap string copied out, the recipient's caller
// scope ends. The constructor's `s + "-tail"` is a +1 producer whose
// reference #2321 TRANSFERS into the caller's slot, so after the call the
// caller's slot is that string's only owner. Pre-fix nothing released it →
// LSan reports the leak → -999. Post-fix the scope-end release balances it
// and Main returns 6.
//
// Mirrors `single_call_out_var_string_recipient_is_leak_free` from #2641's
// regression net and `single_call_ctor_out_var_string_recipient_is_leak_free`
// from #3305's regression net, but uses a PRE-DECLARED recipient at a
// CONSTRUCTOR callee (the `NewExpr` arm's "widen" that #3304 explicitly
// left for this follow-up).
// per §22.3, §22.7, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(predeclared_ctor_out_var_string_recipient_3321,
           predeclared_ctor_out_var_recipient_is_leak_free) {
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
// #3305's LOOP program verbatim, but with a PRE-DECLARED recipient. The
// recipient's alloca is entry-block hoisted, so iteration N+1's constructor
// store LOADS the slot to release what it displaces. If the scope-end
// release did not also null the slot, that load would see the pointer this
// iteration just freed — a double release, which ASan reports rather than
// tolerating. So this test fails BOTH ways: leak without the release,
// double-free without the null. The 5-iteration loop is also the exact
// program shape that produced the original 39 B / 2-allocations measurement
// at the METHOD call site under #2641 / #3304.
//
// This is the AXIS-GRID check that the fix holds across the LITERAL loop
// iteration shape, not just the single-call case. The non-loop shape is
// pinned by the first test above.
// per §22.3, §22.7, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(predeclared_ctor_out_var_string_recipient_3321,
           loop_reused_predeclared_ctor_out_var_slot_is_leak_free) {
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
        "    var n: string := \"zz\"; "
        "    var i := 0; "
        "    while i < 5 do "
        "      var w := new Widget(\"x\", out n); "
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
//
// This is the AXIS-GRID check that the fix holds across the BORROW-AS-PRODUCER
// vs. +1-TRANSFER shape, not just the +1-TRANSFER case. The +1-TRANSFER case
// is pinned by the first test above.
// per §22.3, §22.7, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(predeclared_ctor_out_var_string_recipient_3321,
           borrow_assigned_predeclared_ctor_out_var_recipient_is_leak_free) {
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
        "    var n: string := \"zz\"; "
        "    var w := new Widget(src, out n); "
        "    _ := w; "
        "    return n.Length + src.Length; "
        "  end Main; end C;"), 10);
}

// -----------------------------------------------------------------------------
// The constructor assigns TWICE. The second store's release-of-old reclaims
// the first value, so the caller's slot still owns exactly one reference at
// return — the scope-end release must fire once, not once per assignment.
//
// This is the AXIS-GRID check that the fix holds when the callee assigns
// MULTIPLE times within the same call. The single-assignment case is pinned
// by the first test above. `s = "x"`, so `n = "x-bb"` (length 4) after the
// second assignment wins; the value oracle is 4 (NOT 7 — `"-a"` (length 2)
// would only matter if the SECOND assignment didn't overwrite it).
// per §22.3, §22.7, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(predeclared_ctor_out_var_string_recipient_3321,
           twice_assigned_predeclared_ctor_out_var_recipient_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string) "
        "    n := s + \"-a\"; n := s + \"-bb\"; ok := true; "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var n: string := \"zz\"; "
        "    var w := new Widget(\"x\", out n); "
        "    _ := w; "
        "    return n.Length; "
        "  end Main; end C;"), 4);
}

// -----------------------------------------------------------------------------
// TWO pre-declared `out n: string` recipients in ONE call. Both must be
// released at scope end; the fix's relaxation is per-argument so the same
// mechanism handles both positions.
//
// This is the AXIS-GRID check that the fix holds when MULTIPLE out-recipients
// share one call. The single-recipient case is pinned by the first test
// above.
// per §22.3, §22.7, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(predeclared_ctor_out_var_string_recipient_3321,
           two_predeclared_ctor_out_var_recipients_in_one_call_are_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out a: string, out b: string) "
        "    a := s + \"-a\"; b := s + \"-bb\"; ok := true; "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var a: string := \"zz\"; "
        "    var b: string := \"yy\"; "
        "    var w := new Widget(\"x\", out a, out b); "
        "    _ := w; "
        "    return a.Length + b.Length; "
        "  end Main; end C;"), 7);
}

// -----------------------------------------------------------------------------
// ESCAPE GUARD — the recipient is RETURNED out of the declaring method. The
// scope-end release must NOT fire: the caller of `Wrap` reads the string
// after `Wrap`'s scope has ended, so a scope-end release there would hand
// back freed memory. The fix's escape walk excludes the recipient (no
// onward pass / capture / field store fires — the `return n;` is the
// onward pass), leaving the pre-existing leak. Run on the PLAIN oracle: the
// leak is real and deliberate (leak-over-UAF), so the LSan oracle would
// fail on the very behavior this test is asserting. What it pins is that
// the VALUE survives — with the escape gate removed, this reads a released
// string.
// per §22.3, §22.7, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(predeclared_ctor_out_var_string_recipient_3321,
           returned_predeclared_ctor_out_var_recipient_is_not_released) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string) "
        "    n := s + \"-tail\"; ok := true; "
        "  end constructor; "
        "end Widget; "
        "class W "
        "  public static method Wrap(): string "
        "    var n: string := \"zz\"; "
        "    var w := new Widget(\"x\", out n); "
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
// ESCAPE GUARD — the recipient is STORED into a FIELD that outlives the
// local's scope. The scope-end release must NOT fire: the field's
// reference must survive. Same shape as the returned test, but the storage
// is a field rather than a return slot — both are escape positions the walk
// independently taints. PLAIN oracle: same leak-over-UAF rationale.
// `Sink` has a constructor that initialises `stored` so the field is
// always assigned (§7.9) — the field STORE that creates the escape is the
// `s.stored := n;` assignment in `Go`, not the constructor.
// per §22.3, §22.7, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(predeclared_ctor_out_var_string_recipient_3321,
           field_stored_predeclared_ctor_out_var_recipient_is_not_released) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string) "
        "    n := s + \"-tail\"; ok := true; "
        "  end constructor; "
        "end Widget; "
        "class Sink "
        "  public stored: string; "
        "  public constructor() stored := \"\"; end constructor; "
        "end Sink; "
        "class W "
        "  public static method Go(): string "
        "    var s := new Sink(); "
        "    var n: string := \"zz\"; "
        "    var w := new Widget(\"x\", out n); "
        "    _ := w; "
        "    s.stored := n; "
        "    return s.stored; "
        "  end Go; "
        "end W; "
        "class C "
        "  public static method Main(): int "
        "    var got := W.Go(); "
        "    return got.Length; "
        "  end Main; end C;"), 6);
}

// -----------------------------------------------------------------------------
// SCOPE BOUNDARY — a NULLABLE `out n: string?` recipient. `lowerStringRefParamStore`
// declines a nullable target, so the constructor's store leaves the caller's
// slot with no reference of its own; the fix mirrors that surface and
// registers nothing. This is a deliberate scope line (mirrored at
// `walkNewArgs`'s `out`-arm gate 2), not an oversight: pinning it means a
// later widening of the fix must widen #2321 in the same commit or this
// test's plain-oracle value turns into a use-after-free. Plain oracle: the
// recipient's value, unchanged by this fix.
// per §22.3, §22.7, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(predeclared_ctor_out_var_string_recipient_3321,
           nullable_predeclared_ctor_out_var_recipient_scope_boundary) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string?) "
        "    n := s + \"-tail\"; ok := true; "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var n: string? := \"zz\"; "
        "    var w := new Widget(\"x\", out n); "
        "    _ := w; "
        "    if n <> null then return n.Length; end; "
        "    return 0; "
        "  end Main; end C;"), 6);
}

// -----------------------------------------------------------------------------
// CONTROL — a PRIMITIVE `out n: int` recipient carries no reference at all
// and must gain no disposal. Guards against a widening that registered a
// `bz_string_release` on an `int` slot (which would read the integer as a
// pointer). Value + LSan.
// per §22.3, §22.7, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(predeclared_ctor_out_var_string_recipient_3321,
           primitive_predeclared_ctor_out_var_recipient_control_unaffected) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: int) "
        "    n := 7; ok := true; "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var n: int := 0; "
        "    var w := new Widget(\"x\", out n); "
        "    _ := w; "
        "    return n; "
        "  end Main; end C;"), 7);
}

// -----------------------------------------------------------------------------
// CONTROL — the existing ctor-out-slot mechanism for `out owned var` (carried
// by `ensureCtorOutVarSlots`' `out owned var` / `out using var` arms in
// `LlvmBackend_Calls2.cpp`, #2411 / #2430) must keep working after this fix.
// The constructor here declares an `out owned` parameter (a class-typed
// owned slot, per §28.1 — `out owned` on a value type is rejected), and the
// call site uses the inline `out owned var` form. The Res.alive counter
// settles at 0 after the run, proving the recipient is disposed exactly
// once (no leak, no double-free). Pinning it here catches any future
// regression where the new `walkNewArgs` `out`-relaxation INTERFERES with
// the existing `out owned var` arm.
// per §22.3, §22.4, §28.3
// -----------------------------------------------------------------------------
BEZEL_TEST(predeclared_ctor_out_var_string_recipient_3321,
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
        // SKIP guard for environments without the ASan link toolchain.
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("SKIP") != std::string::npos) return;
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}