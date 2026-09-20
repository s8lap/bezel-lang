// fix #4386 (§22.16): `bezelc --checked-contracts` flag for §22.16
// ensures/invariant evaluation. Pre-#4386 the codegen always emitted
// `ensures`/`invariant` clause evaluations (no flag toggled it); the §22.16
// release default — "in unchecked (release) mode — the default — unproven
// `ensures` and `invariant` clauses are not evaluated" — was unreachable
// from the command line. #4386 adds the `--checked-contracts` flag (parallel
// to `--checked-disposal`) so the §22.16 checked-contracts MODE is a
// per-build choice the user can select.
//
// Spec citation: §22.16 (v0.203, proposal 0043) —
//   * "`ensures` and `invariant` run under checked-contracts mode — a
//      per-build choice between two conformance modes, independent of
//      §28.2's disposal-checking mode: in unchecked (release) mode — the
//      default — unproven `ensures` and `invariant` clauses are not
//      evaluated; in checked-contracts mode they are evaluated at this
//      section's points, a `false` clause raising
//      ContractViolationException."
//   * "`requires` clauses are outside the mode entirely" — the flag must
//      not affect `requires` evaluation; a violated `requires` raises
//      ContractViolationException in every build mode.
//
// Each test exercises the AOT path (fork-isolated Driver::compileFile + clang
// link + native exec) and asserts on the program's exit code (or the
// discriminator a try/catch recovered after a ContractViolationException).
// The CLI flag (--help) test lives in tests/support/checked_contracts_4386_cli_test.cpp
// — it needs to exec the real bezelc binary, which only the support area's
// test partition does.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

// Run in checked-contracts MODE — opts.checkedContracts = true. This is
// exactly `bezelc --checked-contracts` (main.cpp) and matches the pre-#4386
// always-on behavior. A violated ensures/invariant raises
// ContractViolationException at the §22.16 instrumentation point.
int runChecked(const std::string& program) {
    auto r = bezel::test::runAotCheckedContracts(program);
    return r.ran ? r.exitCode : -999;
}

// Run in the §22.16 release default (unchecked mode) — opts is the
// production-default CodegenOptions (checkedContracts = false). `ensures`/
// `invariant` evaluations are NOT emitted at codegen; a violated clause is
// not observed at runtime.
int runUnchecked(const std::string& program) {
    auto r = bezel::test::runAot(program, bezel::codegen::CodegenOptions{});
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// =========================================================================
// Row 1 (§22.16 / --checked-contracts): a violated `ensures(false)` at
// method exit raises ContractViolationException. The catch arm
// discriminates 99. Pins the §22.16 "checked-contracts mode evaluates the
// clause at this section's point" half of the mode toggle.
// =========================================================================
BEZEL_TEST(checked_contracts_4386, with_flag_ensures_violation_raises_spec_22_16) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public method M(): int "
        "    ensures (false) "
        "    return 1; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try "
        "      r := c.M(); "
        "    catch ContractViolationException => r := 99; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 99);
}

// =========================================================================
// Row 2 (§22.16 release default): WITHOUT --checked-contracts, a violated
// `ensures(false)` at method exit does NOT raise — the clause evaluation is
// not emitted at codegen, so the body returns 1 normally. This is the §22.16
// "unchecked (release) mode — the default" behavior the flag selects, and
// the load-bearing FAIL-WITHOUT pin of the fix: pre-#4386 this row could
// not be written at all because the codegen always emitted the clauses.
// =========================================================================
BEZEL_TEST(checked_contracts_4386, without_flag_ensures_violation_skipped_spec_22_16) {
    BEZEL_EXPECT_EQ(runUnchecked(
        "module M; class C "
        "  public method M(): int "
        "    ensures (false) "
        "    return 1; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); return c.M(); "
        "  end Main; "
        "end C;"), 1);
}

// =========================================================================
// Row 3 (§22.16 / --checked-contracts): a violated `invariant(false)` at
// construction exit raises ContractViolationException. The catch arm
// discriminates 77. Pins the construction-exit invariant check under the
// flag — the same point `invariant_member_evaluated_2941_test.cpp`
// exercises under always-on semantics, now reachable via the flag (and
// only via the flag, per Row 2's mirror).
// =========================================================================
BEZEL_TEST(checked_contracts_4386, with_flag_invariant_violation_raises_spec_22_16) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public x: int := 0; "
        "  invariant(x > 0); "
        "  public constructor(v: int) x := v; end constructor; "
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

// =========================================================================
// §22.16 / --checked-contracts orthogonal: a SATISFIED `ensures(true)`
// clause at method exit does NOT raise in either mode — the contract
// machinery evaluates the clause, sees true, and returns normally. Pins
// the §22.16 "the two modes give identical observable behavior for every
// program whose checked clauses never evaluate `false` and never fault"
// invariant for the --checked-contracts bit.
// =========================================================================
BEZEL_TEST(checked_contracts_4386, satisfied_ensures_returns_normally_with_flag_spec_22_16) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public method M(x: int): int "
        "    ensures (x > 0) "
        "    return x; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); return c.M(7); "
        "  end Main; "
        "end C;"), 7);
}

BEZEL_TEST(checked_contracts_4386, satisfied_ensures_returns_normally_without_flag_spec_22_16) {
    BEZEL_EXPECT_EQ(runUnchecked(
        "module M; class C "
        "  public method M(x: int): int "
        "    ensures (x > 0) "
        "    return x; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); return c.M(7); "
        "  end Main; "
        "end C;"), 7);
}

// =========================================================================
// §22.16 "requires clauses are outside the mode entirely": a violated
// `requires(false)` raises ContractViolationException in BOTH modes — the
// flag must NOT elide `requires`. This is the orthogonality guard for the
// `checked-contracts` bit: flipping it on or off must leave `requires`
// evaluation unchanged. Pre-#4386 this row's "without flag" case was
// trivially satisfied (the codegen always evaluated everything); post-#4386
// the unchanged behavior is the load-bearing pin that proves the flag is
// NOT wired to `requires`.
// =========================================================================
BEZEL_TEST(checked_contracts_4386, requires_violation_raises_with_flag_spec_22_16) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public method M(x: int): int "
        "    requires(false) "
        "    return x; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try "
        "      r := c.M(1); "
        "    catch ContractViolationException => r := 11; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 11);
}

BEZEL_TEST(checked_contracts_4386, requires_violation_raises_without_flag_spec_22_16) {
    BEZEL_EXPECT_EQ(runUnchecked(
        "module M; class C "
        "  public method M(x: int): int "
        "    requires(false) "
        "    return x; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try "
        "      r := c.M(1); "
        "    catch ContractViolationException => r := 11; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 11);
}

// =========================================================================
// §22.16 / --checked-contracts orthogonal to --checked-disposal: a
// satisfied disposal-mode test runs under checked-contracts MODE (but NOT
// checked-disposal MODE) — the call returns 42 (no exception), pinning
// the orthogonality. The two modes are independent per-build choices —
// flipping checked-contracts on does not flip checked-disposal.
// =========================================================================
BEZEL_TEST(checked_contracts_4386, flag_orthogonal_to_disposal_spec_22_16) {
    auto r = bezel::test::runAotCheckedContracts(
        "module M; class C "
        "  public method GetValue(): int return 42; end GetValue; "
        "  public static method Main(): int "
        "    using var obj := new C(); "
        "    return obj.GetValue(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
