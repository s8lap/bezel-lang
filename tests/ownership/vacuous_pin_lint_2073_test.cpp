// Spec-derived regression for issue #2073: vacuous-pin lint diagnostic. The
// `using var` keyword admits every RHS shape uniformly (§28.10: "safe to use
// uniformly in generic code … without inspecting the underlying type kind"),
// but two cases make the keyword strictly less expressive than `const`:
//   1. Inert RHS — a value-typed or runtime-managed value carries no disposal
//      obligation (§28.9), so the scope-end `Dispose()` a `using var` would
//      call is a no-op; only the reassignment lock survives.
//   2. Vacuous borrow-pin — a borrow-pin whose source already outlives the
//      enclosing scope (parameter, `this`, static field, or a field chain
//      rooted in such). The pin extends nothing.
// Both cases fire a warning steering the programmer toward `const x := …;`
// (§21.1, v0.201) — the read-only alias with no disposal hint. NEVER an
// error; suppressed in generic bodies where the classification varies per
// instantiation (§28.10's reason for the uniform admissibility).
//
// Spec §28.10 (v0.215):
//   "`using var` admits a value-type or runtime-managed right-hand side. For
//    ordinary value types and runtime-managed types, the disposal at scope
//    end is a no-op (these types do not require disposal per §28.9), and the
//    only effect is the reassignability restriction."
// Rationale §17.8 (v0.201): "`using var` over a borrow/value is an admitted
//    readonly spelling — the pre-existing spelling stops being needed."
//
// Rework (2026-07-22): a method is "generic-bodied" for lint suppression
// when EITHER the method declares its own `<T>` parameters OR the enclosing
// type is generic (`class C<T> method M(c: T) using var x := c`). The
// rework's original-commit `!md->genericParams.empty()` check missed the
// latter — a non-generic method inside a generic class was incorrectly
// fired. Negative-control tests below pin both flavors.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// True iff some WARNING diagnostic fires the inert-RHS vacuous-pin message.
bool hasInertRhsWarning(const OwnershipResult& r, const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("is vacuous") == std::string::npos) continue;
        if (d.message.find("RHS is a value-typed/runtime-managed") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// True iff some WARNING diagnostic fires the vacuous-borrow-pin message.
bool hasVacuousBorrowPinWarning(const OwnershipResult& r,
                                const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("pins a source that already outlives") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// Counts how many times the inert-RHS warning fires for `name`. Mirrors the
// per-declaration emission contract — exactly one per vacuous declaration.
int countInertRhsWarnings(const OwnershipResult& r, const std::string& name) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("is vacuous") == std::string::npos) continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        ++n;
    }
    return n;
}

// Counts how many times the vacuous-borrow-pin warning fires for `name`.
int countVacuousBorrowPinWarnings(const OwnershipResult& r,
                                  const std::string& name) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("pins a source that already outlives") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        ++n;
    }
    return n;
}

// `using var` over a value-typed parameter: §28.9 says value types carry no
// disposal obligation, so the using-var's disposal at scope end is a no-op and
// only the reassignment lock survives — `const` states it better.
BEZEL_TEST(vacuous_pin_lint_2073,
           inert_int_param_steers_to_const_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M(b: int): int "
        "    using var x := b; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_EQ(countInertRhsWarnings(r, "x"), 1);
}

// `using var` over a string literal: runtime-managed (§28.9) → no disposal.
BEZEL_TEST(vacuous_pin_lint_2073,
           inert_string_literal_steers_to_const_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M(): int "
        "    using var s := \"hello\"; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "s"));
    BEZEL_EXPECT_EQ(countInertRhsWarnings(r, "s"), 1);
}

// `using var` over a class-typed borrow parameter: pin extends nothing
// because the parameter already outlives this scope. Spec §28.10 / §28.3.
BEZEL_TEST(vacuous_pin_lint_2073,
           borrow_param_pin_is_vacuous_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(c: Cell): int "
        "    using var x := c; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasVacuousBorrowPinWarning(r, "x"));
    BEZEL_EXPECT_EQ(countVacuousBorrowPinWarnings(r, "x"), 1);
}

// `using var` over `this.field`: receiver outlives the scope, so the field
// read's lifetime is already covered by the receiver's. Pin extends nothing.
BEZEL_TEST(vacuous_pin_lint_2073,
           this_field_pin_is_vacuous_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  private owned _cell: Cell := new Cell(); "
        "  public method M(): int "
        "    using var x := this._cell; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasVacuousBorrowPinWarning(r, "x"));
    BEZEL_EXPECT_EQ(countVacuousBorrowPinWarnings(r, "x"), 1);
}

// Negative control — `using var` over a fresh owned producer IS load-bearing:
// the using-var adopts the disposal obligation. No warning (positive control).
BEZEL_TEST(vacuous_pin_lint_2073,
           fresh_owned_producer_load_bearing_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(): int "
        "    using var c := new Cell(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "c"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "c"));
}

// Negative control — `using var y := owned x` (marked transfer) is a §28.3
// binding-position adoption: load-bearing, no warning. The marker filter
// (`!ld.initializerHasOwnedMarker`) excludes the adoption shape.
BEZEL_TEST(vacuous_pin_lint_2073,
           marked_owned_transfer_load_bearing_no_warn_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(owned c: Cell): int "
        "    using var local := owned c; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "local"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "local"));
}

// Negative control — `using var` over a scope-local borrow: the pin EXTENDS
// the local's lifetime past its natural death. Load-bearing — no warning.
// The rhsIsVacuousBorrowPin helper returns false for LocalSymbol (locals
// never outlive their scope).
BEZEL_TEST(vacuous_pin_lint_2073,
           borrow_local_pin_load_bearing_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(c: Cell): int "
        "    var borrow: Cell := c; "
        "    using var pin := borrow; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "pin"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "pin"));
}

// Negative control — generic method body suppresses BOTH warnings: the
// classification varies per instantiation (§28.10's reason for the uniform
// admissibility), so a value-typed RHS at one instantiation is class-typed
// at another and the lint cannot steer without false positives.
// // per §28.10 #2073
BEZEL_TEST(vacuous_pin_lint_2073,
           generic_method_body_suppresses_lint_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M<T>(t: T): int "
        "    using var x := t; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "x"));
}

// Negative control (rework 2026-07-22) — a non-generic method INSIDE a generic
// class MUST also suppress the lint: the classification of `t: T` varies per
// instantiation (§28.10), so the same source-program can see an inert RHS at
// `C<int>` and a load-bearing one at `C<Cell>` — steering is unsound without
// the per-instantiation information. The original `e61b032` only checked the
// method-level `!md->genericParams.empty()` and missed this case (Opus
// review catch). FAILS WITHOUT THE REWORK.
// // per §28.10 #2073 (rework)
BEZEL_TEST(vacuous_pin_lint_2073,
           generic_class_body_suppresses_lint_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C<T> "
        "  public method M(c: T): int "
        "    using var x := c; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "x"));
}

}  // namespace
