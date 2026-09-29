// Adversarial regression tests for issue #5127 (§28.14, v0.307).
//
// Follow-up from the non-author review of gate-2 fix-forward df9fc1a9, which
// narrowed #5116 (5c407a02)'s widened §28.14 call-bounded closure release in
// LlvmBackend_Calls2.cpp (Emitter::emitMethodCall) to formals whose Named
// head is one of the callee METHOD's own `genericParams` — never an
// enclosing TYPE's own generic parameter. That restriction fixed the
// #1921/#2248 UAF shape for a CLASS-own `T` formal (codegen_1073_closure_arg
// .bare_type_parameter_sink_not_released_spec_28_14, case H), whose header
// comment argues a METHOD-own type param is safe by construction: "no field
// of the enclosing type can be DECLARED at a method's own generic parameter
// … so a method-own-`T` argument can never be stored into a field, a
// collection reachable beyond the call, or returned through any slot that
// does not already name `T` in the callee's own signature."
//
// #5127 asks whether that argument actually holds once the callee widens the
// method-own-typed argument to a type that is NOT `U` (so the field/return
// CAN name it) — `Object`, or a wrapping closure that itself escapes. Each
// row below constructs one such widening and pins the OBSERVED behavior
// under ASan.
//
// MEASURED findings (this worktree, dc/b23-5127, on top of df9fc1a9):
//
//  * A method-own `U` formal, called with an explicit `<() -> int>` type
//    argument and a genuine function-typed caller argument, is accepted by
//    sema only when the callee's OWN return type is non-void (a `void`
//    method-own-`U`-formal call is separately rejected — "argument … not
//    implicitly convertible to the target type (§5.1)" — a plausibly
//    unrelated sema gap, not filed here, out of #5127's scope).
//  * `F := x;` (Case A) where `F: Object?` compiles: storing a method-own
//    generic argument through a widening conversion to `Object` IS
//    reachable, unlike storing to a field of a MISMATCHED concrete type
//    (`F: (() -> int)?`; `x: U`), which sema rejects outright ("the assigned
//    value has a type that is not implicitly convertible to the target
//    type (§5.1)") — so that sink is not constructible at all, matching the
//    #1073/#2248 guard comment's premise for THAT specific shape.
//  * Reading the escaped value back out of the `Object?` field (`F as U` or
//    `F as (() -> int)`) does not currently lower ("internal: … failed to
//    lower to a value" / "#2434" / "#2491") — a separate, pre-existing
//    codegen gap, not exercised here.
//  * Case A (below) is ASan-clean of any use-after-free or double-free: the
//    argument leaks (48 bytes/call, scaling linearly with the loop count —
//    2400 bytes over 50 calls), which is the SAFE direction and matches
//    every other unresolved-escape row in codegen_1073_closure_arg (H, I).
//    The leak is identical whether or not the callee stores `x` at all
//    (verified separately, not committed): the call-bounded release does
//    not differentially engage on this parameter shape, so #5127's
//    predicted premature-release UAF does not reproduce for the `Object`
//    widening sink.
//  * Case B constructs the "captured into another closure that escapes"
//    variant directly within the generic signature (`Wrap<U>(x: U): () -> U`
//    — the wrapping closure's own declared return type names `U`, so no
//    widening or cast is needed to invoke it back at the call site). The
//    existing `declaredReturnOk` gate (LlvmBackend_Calls2.cpp, the
//    `drt->kind == Function` branch) already declines to enter the
//    call-bounded arm at all when the callee's declared return is Function
//    or resolves to Function — so nothing is released, and the only
//    observable cost is the pre-existing 8-byte capture-cell leak per call
//    (the same residual #1073's case I documents for the deferred-return
//    shape), never a UAF.
//
// Conclusion: no use-after-free reproduces for either widening route this
// worktree could construct; both rows pin the SAFE (leaky) outcome under
// ASan so a future regression that turns either leak into a UAF is caught.
//
// Spec: §28.14 (call-bounded default / compiler-verified obligations),
// §28.9 (runtime-managed types), §27.5 (closures). v0.307.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotCheckedLenient;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

}  // namespace

// ===========================================================================
// A — method-own `U` argument widened to an `Object?` field, escaping the
// call. Looped 50x: the pre-existing leak (48 bytes/call = 2400 bytes/50
// calls) scales linearly and ASan reports no use-after-free or double-free.
// Value assertion: `Keep` returns 1 each call, so the loop total pins that
// every call actually ran (not merely compiled away).
// ===========================================================================
BEZEL_TEST(codegen_5127_escape, method_own_type_param_object_field_escape_no_uaf) {
    auto r = runAotCheckedLenient(
        "module M; "
        "class Sink "
        "  public F: Object? := null; "
        "  public method Keep<U>(x: U): int "
        "    F := x; "
        "    return 1; "
        "  end Keep; "
        "end Sink; "
        "class C "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 50 do "
        "      using var s := new Sink(); "
        "      var f: () -> int := lambda () => 42; "
        "      total := total + s.Keep<() -> int>(f); "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 50);
}

// ===========================================================================
// B — method-own `U` argument captured into a NEW closure of declared type
// `() -> U`, which the callee returns directly (the "captured into another
// closure that escapes" shape from the issue, spelled without ever leaving
// the generic type system, so no `Object` cast is needed to invoke the
// result). declaredReturnOk correctly recognizes the Function-kind declared
// return and never enters the call-bounded arm, so `x` is never released
// while `w` still closes over it. Looped 50x; ASan-clean of UAF, and the
// summed return value (50, each closure contributing 1 — kept small so the
// sum cannot exceed the 8-bit exit-code truncation boundary) pins that every
// wrapped closure still invokes the correct original callback after the call
// that produced it has returned.
// ===========================================================================
BEZEL_TEST(codegen_5127_escape, method_own_type_param_wrapped_closure_return_no_uaf) {
    auto r = runAotCheckedLenient(
        "module M; "
        "class Util "
        "  public static method Wrap<U>(x: U): () -> U "
        "    var w: () -> U := lambda () => x; "
        "    return w; "
        "  end Wrap; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 50 do "
        "      var f: () -> int := lambda () => 1; "
        "      var w := Util.Wrap<() -> int>(f); "
        "      total := total + w()(); "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;");
    // fix trap (CLAUDE.md, exit codes > 255 truncate to 8 bits): the closure
    // returns 1 rather than 42 so the 50-call sum (50) stays well under 256 —
    // an earlier revision of this row summed 42 per call (2100 raw), whose
    // truncated exit code (2100 mod 256 = 52) still looked plausible and
    // silently pinned the wrong number until this comment's fix.
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 50);
}

// ===========================================================================
// Control — the plain, single-call form of case B (no loop, no arithmetic
// amplification) with an independent second lambda summed in, pinning the
// exact value from the issue's shape rather than only a loop-scaled one.
// runAot (not checked): this row exists purely to pin the VALUE; the ASan
// net for this exact program shape is already case B above.
// ===========================================================================
BEZEL_TEST(codegen_5127_escape, method_own_type_param_wrapped_closure_return_value) {
    auto r = runAot(std::string(
        "module M; "
        "class Util "
        "  public static method Wrap<U>(x: U): () -> U "
        "    var w: () -> U := lambda () => x; "
        "    return w; "
        "  end Wrap; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var f: () -> int := lambda () => 42; "
        "    var w := Util.Wrap<() -> int>(f); "
        "    var g := lambda () => 7; "
        "    return w()() + g(); "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 49);
}
