// Spec-derived regression tests for issue #2410 — `lowerNullCondCall` (the
// `?.` call lowering, LlvmBackend_Calls2.cpp) never hoisted a skippable
// out-var producer NESTED IN ITS OWN CALL'S ARGUMENTS before the
// receiver-null short-circuit.
//
// BACKGROUND: #2395 (§7.9/§22.4) added `hoistSkippableOutVarSlots` so that a
// nullable inline `out var` recipient inside an `and`/`or` right operand gets
// its slot pre-created/zero-stored BEFORE the short-circuit branch (a
// `lowerShortCircuit` call to `hoistSkippableOutVarSlots(*bin.right)`), so a
// read on the skip path observes the §7.9 default `null` rather than stack
// garbage. #2345 then gave `lowerNullCondCall` its OWN call to
// `ensureOutVarSlots(call)` — but that only pre-creates/zero-stores the
// call's OWN top-level `out var` recipients; it does NOT recurse into the
// call's ARGUMENTS. So a skippable out-var producer nested in a `?.`-guarded
// call's argument list — `recv?.Foo(Other.TryParse("x", out var n))` or
// `recv?.Foo(new Widget("x", out var n).ok)` — had its zero-store emitted
// from inside `callBB` (the receiver-non-null continuation), which the
// null-receiver short-circuit (`endBB` predecessor) never executes: a read of
// `n` past the `if` on the call-skipped path observed stack garbage instead
// of the §7.9 default `null`.
//
// THE FIX (lowerNullCondCall): run the SAME idempotent
// `hoistSkippableOutVarSlots` over every argument RIGHT AFTER the existing
// `ensureOutVarSlots(call)` (and before the receiver-null branch is emitted),
// mirroring `lowerShortCircuit`'s placement. §7.5 ("if any segment of a
// chain is null, the remainder is not evaluated") governs ARGUMENT
// EVALUATION — the call's argument expressions still lower only inside
// `callBB` — not the dominating SLOT CREATION this hoist performs, exactly
// the distinction #2395 already established for `and`/`or`. Confirmed live
// (both the plain-call-arg and constructor-arg variants) on current main
// with #2395/#2402 applied: both return the "spuriously non-null" branch
// (exit 2) without the fix and the §7.9 default-null branch (exit 3) with it.
//
// This file pins the fix with a value-asserting matrix:
//   * the null-receiver (call-skipped) path for both the plain-call-arg and
//     constructor-arg variants — the bug's own repro, FAILS without the fix;
//   * the non-null-receiver (call-ran) control for both flavors — proves the
//     hoist does not disturb the existing call-ran value flow;
//   * an argument-side-effect probe — the nested producer must NOT run on
//     the null-skip path (§7.5: argument evaluation is skipped), only its
//     SLOT CREATION is hoisted;
//   * multiple skippable args in one call;
//   * a `?.`-chained receiver (`a?.B?.Foo(...)`) — two short-circuit points.

#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}


}  // namespace

// =============================================================================
// Plain-call-arg flavor: `recv?.Foo(C.TryParse("x", out var n))`. The nested
// `TryParse` is itself a skippable out-var producer whose slot must be
// hoisted before the receiver-null branch. NULL receiver → call skipped →
// `n` defaults to `null` (§7.9) → Main returns 3. Without the fix the
// zero-store landed inside `callBB` (never executed on this path) and `n`'s
// hasValue tag read stack garbage (non-null) → Main returned 2.
// =============================================================================

BEZEL_TEST(codegen_nullcond_nested_outvar_arg_2410,
           plain_call_arg_null_receiver_out_var_defaults_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method TryParse(s: string, out n: int?): bool "
        "    n := 42; return true; "
        "  end TryParse; "
        "  public method Foo(x: bool): bool return x; end Foo; "
        "  public static method Main(): int "
        "    var recv: C? := null; "
        "    if recv?.Foo(C.TryParse(\"x\", out var n)) = true then "
        "      return 1; "
        "    end; "
        "    if n <> null then return 2; end; "
        "    return 3; "
        "  end Main; end C;"), 3);
}

// Non-null control for the plain-call-arg flavor: the call runs, `TryParse`
// sets `n := 42` and returns true, `Foo(true)` yields true → Main returns 1.
// Proves the hoist did not disturb the call-ran value flow.
BEZEL_TEST(codegen_nullcond_nested_outvar_arg_2410,
           plain_call_arg_nonnull_receiver_out_var_assigned) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method TryParse(s: string, out n: int?): bool "
        "    n := 42; return true; "
        "  end TryParse; "
        "  public method Foo(x: bool): bool return x; end Foo; "
        "  public static method Main(): int "
        "    using var recvObj := new C(); "
        "    var recv: C? := recvObj; "
        "    if recv?.Foo(C.TryParse(\"x\", out var n)) = true then "
        "      return 1; "
        "    end; "
        "    if n <> null then return 2; end; "
        "    return 3; "
        "  end Main; end C;"), 1);
}

// =============================================================================
// Constructor-arg flavor: `recv?.Foo(new Widget("x", out var n).ok)`. The
// nested `new Widget(...)` is a constructor out-var producer (a MemberAccess
// wrapping a NewExpr) whose slot must be hoisted. Same matrix as above.
// =============================================================================

BEZEL_TEST(codegen_nullcond_nested_outvar_arg_2410,
           ctor_arg_null_receiver_out_var_defaults_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: int?) "
        "    n := 42; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public method Foo(x: bool): bool return x; end Foo; "
        "  public static method Main(): int "
        "    var recv: C? := null; "
        "    if recv?.Foo(new Widget(\"x\", out var n).ok) = true then "
        "      return 1; "
        "    end; "
        "    if n <> null then return 2; end; "
        "    return 3; "
        "  end Main; end C;"), 3);
}

BEZEL_TEST(codegen_nullcond_nested_outvar_arg_2410,
           ctor_arg_nonnull_receiver_out_var_assigned) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: int?) "
        "    n := 42; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public method Foo(x: bool): bool return x; end Foo; "
        "  public static method Main(): int "
        "    using var recvObj := new C(); "
        "    var recv: C? := recvObj; "
        "    if recv?.Foo(new Widget(\"x\", out var n).ok) = true then "
        "      return 1; "
        "    end; "
        "    if n <> null then return 2; end; "
        "    return 3; "
        "  end Main; end C;"), 1);
}

// =============================================================================
// §7.5 argument-elevation guard: the hoist moves only SLOT CREATION, not
// argument EVALUATION. On the null-skip path the nested producer must NOT
// run — its observable side effect (a static counter bump) stays at 0 and
// `n` stays null. (Without the fix this same program returned 2 — the
// garbage-tag read — AND would have returned 2 here too; with the fix the
// counter correctly stays 0, proving the producer body did not execute even
// though its recipient slot was pre-created.)
// =============================================================================

BEZEL_TEST(codegen_nullcond_nested_outvar_arg_2410,
           producer_side_effect_not_run_on_null_skip_path) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static Counter: int := 0; "
        "  public static method TryParse(s: string, out n: int?): bool "
        "    C.Counter := C.Counter + 1; "
        "    n := 42; return true; "
        "  end TryParse; "
        "  public method Foo(x: bool): bool return x; end Foo; "
        "  public static method Main(): int "
        "    var recv: C? := null; "
        "    if recv?.Foo(C.TryParse(\"x\", out var n)) = true then "
        "      return 1; "
        "    end; "
        "    if C.Counter <> 0 then return 8; end; "
        "    if n <> null then return 2; end; "
        "    return 3; "
        "  end Main; end C;"), 3);
}

// Non-null counterpart: on the call-ran path the producer DOES run and bumps
// the counter — proves the side-effect suppression above is a §7.5 skip
// effect, not a bug that never runs the producer at all.
BEZEL_TEST(codegen_nullcond_nested_outvar_arg_2410,
           producer_side_effect_runs_on_nonnull_path) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static Counter: int := 0; "
        "  public static method TryParse(s: string, out n: int?): bool "
        "    C.Counter := C.Counter + 1; "
        "    n := 42; return true; "
        "  end TryParse; "
        "  public method Foo(x: bool): bool return x; end Foo; "
        "  public static method Main(): int "
        "    using var recvObj := new C(); "
        "    var recv: C? := recvObj; "
        "    if recv?.Foo(C.TryParse(\"x\", out var n)) = true then "
        "      if C.Counter <> 1 then return 9; end; "
        "      if n <> 42 then return 7; end; "
        "      return 1; "
        "    end; "
        "    return 4; "
        "  end Main; end C;"), 1);
}

// =============================================================================
// Multiple skippable out-var producers in one call's arguments: BOTH must be
// hoisted. Null receiver → both `m` and `n` default to null → Main returns 3.
// =============================================================================

BEZEL_TEST(codegen_nullcond_nested_outvar_arg_2410,
           two_skippable_out_var_args_null_receiver_both_default_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method P(s: string, out n: int?): bool "
        "    n := 42; return true; "
        "  end P; "
        "  public static method Q(s: string, out n: int?): bool "
        "    n := 43; return true; "
        "  end Q; "
        "  public method Foo(x: bool, y: bool): bool return x and y; end Foo; "
        "  public static method Main(): int "
        "    var recv: C? := null; "
        "    if recv?.Foo(C.P(\"x\", out var m), C.Q(\"y\", out var n)) = true then "
        "      return 1; "
        "    end; "
        "    if m <> null then return 2; end; "
        "    if n <> null then return 5; end; "
        "    return 3; "
        "  end Main; end C;"), 3);
}

// =============================================================================
// Chained null-conditional receiver (`a?.B?.Foo(...)`): TWO short-circuit
// points (`a` null, `a.B` null). The nested producer in `Foo`'s arg must be
// hoisted past BOTH. Each null combination defaults `n` to null.
// =============================================================================

namespace {
const char* kChain2410Classes =
    "module M; "
    "class Inner public method Foo(x: bool): bool return x; end Foo; end Inner; "
    "class Outer public owned B: Inner? := null; end Outer; ";
}  // namespace

BEZEL_TEST(codegen_nullcond_nested_outvar_arg_2410,
           chained_nullcond_a_null_out_var_defaults_null) {
    BEZEL_EXPECT_EQ(run(std::string(kChain2410Classes) +
        "class C "
        "  public static method TryParse(s: string, out n: int?): bool "
        "    n := 42; return true; "
        "  end TryParse; "
        "  public static method Main(): int "
        "    var a: Outer? := null; "
        "    if a?.B?.Foo(C.TryParse(\"x\", out var n)) = true then "
        "      return 1; "
        "    end; "
        "    if n <> null then return 2; end; "
        "    return 3; "
        "  end Main; end C;"), 3);
}

BEZEL_TEST(codegen_nullcond_nested_outvar_arg_2410,
           chained_nullcond_b_null_out_var_defaults_null) {
    BEZEL_EXPECT_EQ(run(std::string(kChain2410Classes) +
        "class C "
        "  public static method TryParse(s: string, out n: int?): bool "
        "    n := 42; return true; "
        "  end TryParse; "
        "  public static method Main(): int "
        "    using var aa := new Outer(); "  // aa.B stays null
        "    var a: Outer? := aa; "
        "    if a?.B?.Foo(C.TryParse(\"x\", out var n)) = true then "
        "      return 1; "
        "    end; "
        "    if n <> null then return 2; end; "
        "    return 3; "
        "  end Main; end C;"), 3);
}

// =============================================================================
// Non-skippable argument is unaffected: a plain (non-out-var) argument
// alongside the skippable producer still lowers normally on both paths. Null
// receiver → producer skipped, plain arg never evaluated either (§7.5), `n`
// null, and the plain arg's side effect (if any) also suppressed — covered
// by the side-effect test above. This case isolates that a plain `int`
// second argument does not break the hoist of the first.
// =============================================================================

BEZEL_TEST(codegen_nullcond_nested_outvar_arg_2410,
           non_skippable_arg_alongside_skippable_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method TryParse(s: string, out n: int?): bool "
        "    n := 42; return true; "
        "  end TryParse; "
        "  public method Foo(x: bool, d: int): bool return x; end Foo; "
        "  public static method Main(): int "
        "    using var recvObj := new C(); "
        "    var recv: C? := recvObj; "
        "    if recv?.Foo(C.TryParse(\"x\", out var n), 99) = true then "
        "      if n <> 42 then return 7; end; "
        "      return 1; "
        "    end; "
        "    return 4; "
        "  end Main; end C;"), 1);
}
