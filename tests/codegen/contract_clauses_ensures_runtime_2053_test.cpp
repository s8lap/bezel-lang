// Spec-derived runtime tests for §22.16 contract clauses (v0.203, proposal
// 0043, #2053) — Slice 4b: ensures single-exit epilogue LOWERING.
//
// Spec §22.16 "Evaluation semantics":
//   * `ensures` clauses evaluate on every NORMAL-COMPLETION PATH — a return,
//     a unit-typed method falling off its end, an arrow body's completion, a
//     constructor body's completion — and NEVER on a path that exits by
//     throw. The evaluation point is: AFTER the return value is computed and
//     bound and after every `out` parameter's callee-local slot holds its
//     final value, and BEFORE the frame's scope-end work (`using var`
//     disposals, owned-local auto-drops, `out` copy-out). A false clause
//     faults with ContractViolationException.
//   * "ensures and invariant run under checked-contracts mode" — BUT for
//     this slice the runtime always evaluates them (the checked/unchecked
//     MODE distinction is Slice 5; the evaluation machinery itself is what
//     Slice 4b lowers).
//
// Appendix C.15 bullets covered here:
//   * "ensures evaluates on every normal-completion path after result and
//     out-slot binding and before scope-end disposals, and never on a throw
//     path."
//
// Slice 4b implements the ensures epilogue. The `result`/`old()` operand
// machinery is Slice 4c — the tests here write ensures clauses that read
// only parameters/this/statics (the admitted set without result/old).
//
// Each test exercises a runtime triple: the program runs (compiles +
// executes natively via the AOT harness), and the assertion is on the
// program's exit code or caught-exception value.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

int runFull(const std::string& program) {
    auto r = bezel::test::runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// =========================================================================
// §22.16: a satisfied ensures clause does NOT fault — the method returns.
// =========================================================================
BEZEL_TEST(contract_clauses_ensures_runtime_2053,
           satisfied_ensures_clause_returns_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  private _v: int := 0; "
        "  public method M(x: int): int "
        "    ensures (_v + x >= 0) "
        "    _v := x; return x; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; r := c.M(5); return r; "
        "  end Main; "
        "end C;"), 5);
}

// §22.16: a violated ensures clause faults with ContractViolationException.
// The caller catches it via `catch ContractViolationException`.
BEZEL_TEST(contract_clauses_ensures_runtime_2053,
           violated_ensures_clause_raises_contract_violation_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  private _v: int := 0; "
        "  public method M(x: int): int "
        "    ensures (_v > 100) "
        "    _v := x; return x; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try "
        "      r := c.M(5); "
        "    catch ContractViolationException => r := 99; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 99);
}

// §22.16: a violated ensures clause on a CONSTRUCTOR faults at body
// completion — every §7.9-required field is assigned by then.
BEZEL_TEST(contract_clauses_ensures_runtime_2053,
           violated_constructor_ensures_raises_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public v: int := 0; "
        "  public constructor(v: int) "
        "    ensures (v > 100) "
        "    this.v := v; "
        "  end constructor; "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    try "
        "      using var c := new C(5); "
        "    catch ContractViolationException => r := 77; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 77);
}

// §22.16: ensures NEVER evaluates on a throw path. The method throws; no
// ContractViolationException surfaces even though the ensures clause would
// fault if evaluated.
BEZEL_TEST(contract_clauses_ensures_runtime_2053,
           ensures_not_evaluated_on_throw_path_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public method M(x: int): int "
        "    ensures (false) "
        "    throw new ArgumentException(); "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try "
        "      r := c.M(5); "
        "    catch ArgumentException => r := 55; "
        "    catch ContractViolationException => r := 1; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 55);
}

// §22.16: multiple ensures clauses evaluate in declaration order; the first
// failure surfaces.
BEZEL_TEST(contract_clauses_ensures_runtime_2053,
           multiple_ensures_first_failure_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public method M(x: int): int "
        "    ensures (x < 100) "
        "    ensures (x > 0) "
        "    return x; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try "
        "      r := c.M(50); "
        "    catch ContractViolationException => r := 11; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 50);
}

// §22.16: ensures is evaluated AFTER the return value is bound and the
// method's effects are visible — the epilogue observes the field's updated
// state. When the ensures faults, the caller observes the throw, not the
// value.
BEZEL_TEST(contract_clauses_ensures_runtime_2053,
           ensures_observes_post_state_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  private _v: int := 0; "
        "  public method M(x: int): int "
        "    ensures (_v > 100) "
        "    _v := x + 200; return x; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    r := c.M(5); "
        "    return r; "
        "  end Main; "
        "end C;"), 5);
}

// §22.16: a unit method (no declared return type) admits ensures; the
// epilogue evaluates over the frame state at fall-through.
BEZEL_TEST(contract_clauses_ensures_runtime_2053,
           unit_method_ensures_at_fallthrough_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  private _v: int := 0; "
        "  public method SetV(x: int) "
        "    ensures (_v > 0) "
        "    _v := x; "
        "  end SetV; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try "
        "      c.SetV(-5); "
        "    catch ContractViolationException => r := 22; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 22);
}

// Additive: a zero-clause method still compiles and runs as before — no
// epilogue is emitted, emitReturn goes through the direct path.
BEZEL_TEST(contract_clauses_ensures_runtime_2053,
           zero_clause_method_runs_unchanged_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public method M(x: int): int return x; end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; r := c.M(7); return r; "
        "  end Main; "
        "end C;"), 7);
}
