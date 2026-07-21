// Spec-derived runtime tests for §22.16 contract clauses (v0.207, proposal
// 0055, #2078) — Slice 4a: requires-prologue LOWERING. Clauses use the
// structural-paren surface `requires(E)` / `ensures(E)` (proposal 0055).
//
// Spec §22.16 "Evaluation semantics":
//   * `requires` clauses evaluate on every invocation, AFTER argument binding
//     completes and BEFORE any of the member's own execution — before a
//     constructor's base(...)/this(...) chain, before field initializers,
//     before the first body statement — in declaration order, stopping at the
//     first clause producing false, which faults with ContractViolationException.
//   * "requires violations are always-on" (§22.16 "Failure semantics and the
//     enforcement modes"): an unproven requires clause is compiled as a runtime
//     check in every build mode; it is never elided.
//
// Appendix C.15 bullets covered here:
//   * "requires evaluates after argument binding and before the chain/
//     initializers/body, in declaration order, and is never elided unless
//     proven."
//
// Slice 4a implements the requires prologue only — the single-exit ensures
// epilogue, old() capture, and result binding land in subsequent sub-slices.
//
// Each test exercises a runtime triple: the program runs (compiles + executes
// natively via the AOT harness), and the assertion is on the program's exit
// code or caught-exception value.

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
// §22.16: a satisfied requires clause does NOT fault — the body runs.
// =========================================================================
BEZEL_TEST(contract_clauses_requires_runtime_2053,
           satisfied_requires_clause_runs_body_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public method M(x: int): int "
        "    requires(x > 0) "
        "    return x; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; r := c.M(42); return r; "
        "  end Main; "
        "end C;"), 42);
}

// §22.16: a violated requires clause faults with ContractViolationException.
// The caller catches it via `catch ContractViolationException`.
BEZEL_TEST(contract_clauses_requires_runtime_2053,
           violated_requires_clause_raises_contract_violation_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public method M(x: int): int "
        "    requires(x > 0) "
        "    return x; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try "
        "      r := c.M(-1); "
        "    catch ContractViolationException => r := 99; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 99);
}

// §22.16: a violated requires clause on a CONSTRUCTOR faults BEFORE the body
// runs (no side effects of construction observable).
BEZEL_TEST(contract_clauses_requires_runtime_2053,
           violated_constructor_requires_raises_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public v: int := 0; "
        "  public constructor(v: int) "
        "    requires(v > 0) "
        "    this.v := v; "
        "  end constructor; "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    try "
        "      using var c := new C(-1); "
        "    catch ContractViolationException => r := 77; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 77);
}

// §22.16 "declaration order, stop at first false": multiple requires clauses,
// the first one fails — the second is not evaluated (its side-effect-free
// anyway, but the fault is observed).
BEZEL_TEST(contract_clauses_requires_runtime_2053,
           multiple_requires_first_fails_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public method M(x: int): int "
        "    requires(x > 100) "
        "    requires(x < 5) "
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
        "end C;"), 11);
}

// §22.16: ContractViolationException derives from Exception per the §31.4
// hierarchy — a generic `catch Exception` arm catches it.
BEZEL_TEST(contract_clauses_requires_runtime_2053,
           contract_violation_is_exception_subtype_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public method M(x: int): int "
        "    requires(x > 0) "
        "    return x; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try "
        "      r := c.M(-1); "
        "    catch Exception => r := 33; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 33);
}

// §22.16: requires reads `this`'s fields (the borrow is admitted). The
// clause observes the current state and faults when the call would violate
// the invariant the clause states.
BEZEL_TEST(contract_clauses_requires_runtime_2053,
           requires_reads_instance_field_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  private _max: int := 10; "
        "  public method Add(x: int): int "
        "    requires(x <= _max) "
        "    return x; "
        "  end Add; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try "
        "      r := c.Add(100); "
        "    catch ContractViolationException => r := 22; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 22);
}

// Additive: a zero-clause method still compiles and runs as before — no
// prologue is emitted.
BEZEL_TEST(contract_clauses_requires_runtime_2053,
           zero_clause_method_runs_unchanged_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public method M(x: int): int return x; end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; r := c.M(7); return r; "
        "  end Main; "
        "end C;"), 7);
}
