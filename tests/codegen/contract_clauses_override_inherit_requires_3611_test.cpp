// fix #3611 (§22.16 / §12.1, v0.286): contract clauses are "inherited, never
// restated" by an override — a `requires` on a base method MUST fire when an
// override is invoked (the inherited precondition holds for the override too),
// and an override is forbidden from declaring its own `requires` clauses
// (§22.16 "an override or implementation may add `ensures` clauses and may not
// declare `requires` clauses"). Two probe pairs — the inheritance half and
// the rejection half — pinned as `FAIL-WITHOUT / PASS-WITH` of #3611.
//
// Spec citation, verbatim: §22.16 (v0.286) — "Clauses bind overrides and
// implementations ([§12.1], [§11.2]): they are inherited, never restated;
// an override or implementation may add `ensures` clauses and may not
// declare `requires` clauses."
//
// Both probes pre-fix:
//   * inheritance: `d.Half(-4)` EXITED with the half result, not 99 — the
//     base's `requires(x > 0)` never fired at the override's call site
//     because the override's compiled prologue was empty
//     (MemberSymbol::contractRecords had no consumers, and the BASE's AST
//     was never rebound into the OVERRIDE's prologue).
//   * restated-requires: a derived class adding its own `requires` to an
//     override compiled clean (exit 0), silently admitting the §22.16
//     violation.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

int runFull(const std::string& program) {
    auto r = bezel::test::runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// =========================================================================
// §22.16 / §12.1: an OVERRIDE inherits the base member's `requires` clause.
// A negative-argument call dispatches to the override body, so without
// inheritance the override's prologue is empty and the base's precondition
// never fires — `d.Half(-4)` would silently divide a negative number.
// Post-#3611 the override's prologue lowers the base's requires, fires
// ContractViolationException, and the catch arm returns 99.
// =========================================================================
BEZEL_TEST(contract_clauses_override_inherit_requires_3611,
           override_inherits_base_requires_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "public class Base "
        "  public virtual method Half(x: int): int "
        "    requires(x > 0) "
        "    return x / 2; "
        "  end Half; "
        "end Base; "
        "public class Derived : Base "
        "  public override method Half(x: int): int "
        "    return x / 2; "
        "  end Half; "
        "end Derived; "
        "public class App "
        "  public static method Main(): int "
        "    var d: Derived := new Derived(); "
        "    try "
        "      var r: int := d.Half(-4); "
        "      return 1; "
        "    catch ContractViolationException => return 99; "
        "    end; "
        "  end Main; "
        "end App;"), 99);
}

// §22.16 / §12.1: an override with an INHERITED `requires` that the caller
// satisfies MUST still run the body — the inheritance is on every call, not
// only on the violating one. Pre-#3611 this passed by accident (the override
// had no prologue so it ran the body regardless); pinning the success path
// guards against a fix that over-corrects and rejects every call.
BEZEL_TEST(contract_clauses_override_inherit_requires_3611,
           override_inheriting_base_requires_runs_body_when_satisfied) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "public class Base "
        "  public virtual method Half(x: int): int "
        "    requires(x > 0) "
        "    return x / 2; "
        "  end Half; "
        "end Base; "
        "public class Derived : Base "
        "  public override method Half(x: int): int "
        "    return x / 2; "
        "  end Half; "
        "end Derived; "
        "public class App "
        "  public static method Main(): int "
        "    var d: Derived := new Derived(); "
        "    return d.Half(8); "
        "  end Main; "
        "end App;"), 4);
}

// §22.16: an override MUST NOT restate a `requires` clause. Pre-#3611 the
// sema passed and the program compiled clean; post-#3611 the diagnostic
// names the override and cites §22.16. The harness's `r.diagnostics` is
// only readable on `!CompileOutcome::Ok` (per aot_support.hpp — stderr is
// DISCARDED on a successful compile, so a vacuous "no error" assertion on
// OK would not pin anything).
BEZEL_TEST(contract_clauses_override_inherit_requires_3611,
           override_restating_requires_is_rejected_spec_22_16) {
    // The restated-requires case goes through bezelc DIRECTLY (not through
    // runAot) so we can read `r.diagnostics` regardless of compile status.
    auto r = bezel::test::runAot(
        "module M; "
        "public class Base "
        "  public virtual method Half(x: int): int "
        "    requires(x > 0) "
        "    return x / 2; "
        "  end Half; "
        "end Base; "
        "public class Derived : Base "
        "  public override method Half(x: int): int "
        "    requires(x > 0) "
        "    return x / 2; "
        "  end Half; "
        "end Derived; "
        "public class App "
        "  public static method Main(): int => 0; "
        "end App;");
    BEZEL_EXPECT_FALSE(r.ran);  // compile must fail (diagnostic on override)
    // The diagnostic must name the override and the §22.16 rule — exact text,
    // not just hasErrors() (which a future regression could trip silently
    // with an unrelated diagnostic).
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("override of `Half`") != std::string::npos &&
            d.message.find("requires") != std::string::npos &&
            d.message.find("\xC2\xA7""22.16") != std::string::npos) {
            found = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(found);
}

// §22.16 / §12.1: TWO overrides of the same base — each one independently
// inherits the base's requires. The first override's violation and the
// second override's success exercise the inheritance on two different
// derived classes in one compilation unit (so the rebind machinery runs
// over TWO independent MemberSymbol entries in the same pass).
BEZEL_TEST(contract_clauses_override_inherit_requires_3611,
           two_overrides_inherit_same_base_requires_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "public class Base "
        "  public virtual method Half(x: int): int "
        "    requires(x > 0) "
        "    return x / 2; "
        "  end Half; "
        "end Base; "
        "public class D1 : Base "
        "  public override method Half(x: int): int "
        "    return x / 2; "
        "  end Half; "
        "end D1; "
        "public class D2 : Base "
        "  public override method Half(x: int): int "
        "    return x / 4; "
        "  end Half; "
        "end D2; "
        "public class App "
        "  public static method Main(): int "
        "    var a: D1 := new D1(); "
        "    var b: D2 := new D2(); "
        "    var r1: int := -1; var r2: int := -1; "
        "    try r1 := a.Half(-2); catch ContractViolationException => r1 := 88; end; "
        "    try r2 := b.Half(8); catch ContractViolationException => r2 := 88; end; "
        "    return r1 + r2; "
        "  end Main; "
        "end App;"), 88 + 2);
}
