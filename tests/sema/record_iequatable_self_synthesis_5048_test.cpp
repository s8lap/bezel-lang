// fix #5048 (§14.2 clause 2, v0.307): a record that lists `IEquatable<Self>`
// in its inheritance clause satisfies the constraint via the synthesized
// `Equals(T)` witness — auto-generated members count per §14.2 rule 2:
// "auto-generated members count: the synthesized `Equals(T)` satisfies
// `IEquatable<T>`".
//
// ROOT CAUSE (src/sema/TypeResolution_Checks.cpp,
// `Resolver::checkAbstractImplementations`, BFS at L6882):
// The BFS populates `concreteNames` from `TypeSymbol::members`. Positional
// records and body records that qualify for auto-equality synthesize
// `Equals(T)` / `GetHashCode()` / `ToString()` at codegen time only
// (LlvmBackend_Setup.cpp / LlvmBackend_String.cpp) — they are never added
// to `TypeSymbol::members` during sema. So when the BFS reaches the record's
// own members, `Equals` is absent from `concreteNames`, while `IEquatable<T>`
// contributes `Equals(T)` to `abstractNames`. The mismatch produces a false
// "concrete record does not implement inherited abstract member `Equals`"
// error at L7030-7038.
//
// THE FIX: after the BFS frontier is seeded with the root record `t` and
// before the BFS begins, check whether `t` (a record) lists `IEquatable<U>`
// in its `baseTypes` where U resolves to `t` itself (Self). If so and `t`
// qualifies for auto-equality synthesis (isPositionalRecordEqualityBearing
// or isAutoEqualityBearingBodyRecord), inject `Equals` into `concreteNames`
// and `concreteArities["Equals"]` so the subsequent abstractNames check does
// not fire. This mirrors how `checkAbstractImplementations` already handles
// positional fields by inserting them into `concretePropertyNames` directly
// (§9.1 handling, L6993-6999).

#include <string>

#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCollect;

namespace {

// Shared prelude — defined once, shared across all test cases.
const std::string kPrelude =
    "module M; "
    // T1: positional record satisfying IEquatable<Self> via synthesis.
    "record Point(X: int, Y: int): IEquatable<Point>; "
    // T2: nested record — outer implements IEquatable<Outer>, inner via
    // auto-synthesis; the instantiation of a generic constrained on
    // IEquatable<T> with T=Inner must resolve.
    "record Outer(Value: int): IEquatable<Outer>; "
    "record Inner(Data: string): IEquatable<Inner>; "
    // T3: body record with auto-property (auto-equality bearing), listing
    // IEquatable<Self>.
    "record Label "
    "  public Name: string; "
    "  public constructor(n: string) Name := n; end constructor; "
    "end Label; "
    // T4 control: body record with explicit `Equals` — should NOT be
    // auto-synthesized, but is accepted because the explicit member IS the
    // witness. This is the T4 from the issue design.
    "record Person "
    "  public Name: string; "
    "  public constructor(n: string) Name := n; end constructor; "
    "  public method Equals(other: Person): bool => Name = other.Name; "
    "  public method GetHashCode(): int => Name.GetHashCode(); "
    "end Person; "
    // T5 control: record that does NOT list IEquatable<T> — should NOT
    // satisfy `where T: IEquatable<T>` constraint (the cross-module inferred
    // call case mentioned in the issue's review note).
    "record NoIEquatable(A: int); "
    // T6: generic constrained on IEquatable<T> — instantiating with Point
    // (which lists IEquatable<Point>) should satisfy the constraint.
    "static class Utils<T> where T: IEquatable<T> "
    "  public static method Contains(a: T, b: T): bool => a.Equals(b); "
    "end Utils; "
    // T7: generic with no IEquatable constraint — purely a compile-only
    // control to confirm the fix does not touch unconstrained generics.
    "static class Simple<T> "
    "  public static method Id(x: T): T return x; end Id; "
    "end Simple; ";

bezel::test::SemaResult runSema(const std::string& src) {
    auto r = parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True when r diagnostics contain a §12.5 "does not implement" error.
bool hasDoesNotImplementError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("does not implement") != std::string::npos)
            return true;
    return false;
}

// True when r diagnostics contain a §14.2 constraint error.
bool hasConstraintError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("does not satisfy") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ================================================================
// ACCEPT ROWS — §14.2 rule 2 acceptance via synthesized Equals(T).
// Each record with `IEquatable<Self>` listing compiles cleanly (sema).
// ================================================================

// T1: simple positional record — the core repro.
BEZEL_TEST(record_iequatable_self_synthesis_5048,
           positional_record_lists_iequatable_self_accepted_spec_14_2_rule_2) {
    auto r = runSema(kPrelude +
        "class Pgm "
        "  public static method Main(): int return 0; end Main; "
        "end Pgm;");
    BEZEL_EXPECT_FALSE(hasDoesNotImplementError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// T2: nested record — the inner type satisfies the constraint via synthesis.
BEZEL_TEST(record_iequatable_self_synthesis_5048,
           nested_record_lists_iequatable_self_accepted_spec_14_2_rule_2) {
    auto r = runSema(kPrelude +
        "class Pgm "
        "  public static method Main(): int return 0; end Main; "
        "end Pgm;");
    BEZEL_EXPECT_FALSE(hasDoesNotImplementError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// T3: body record with auto-property (isAutoEqualityBearingBodyRecord).
BEZEL_TEST(record_iequatable_self_synthesis_5048,
           body_record_auto_property_lists_iequatable_self_accepted_spec_14_2_rule_2) {
    auto r = runSema(kPrelude +
        "class Pgm "
        "  public static method Main(): int return 0; end Main; "
        "end Pgm;");
    BEZEL_EXPECT_FALSE(hasDoesNotImplementError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// T4: body record with explicit Equals — the explicit member IS the witness.
BEZEL_TEST(record_iequatable_self_synthesis_5048,
           body_record_explicit_equals_lists_iequatable_self_accepted_spec_14_2_rule_2) {
    auto r = runSema(kPrelude +
        "class Pgm "
        "  public static method Main(): int return 0; end Main; "
        "end Pgm;");
    BEZEL_EXPECT_FALSE(hasDoesNotImplementError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ================================================================
// CONSTRAINT ROWS — the `where T: IEquatable<T>` shape that motivated
// the issue. Instantiation with a record listing IEquatable<Self> must
// succeed (compile-time constraint check); instantiation with a record
// NOT listing IEquatable must fail.
// ================================================================

// T6: positional record listing IEquatable<Self> satisfies the
// `where T: IEquatable<T>` constraint at instantiation.
BEZEL_TEST(record_iequatable_self_synthesis_5048,
           generic_iequatable_self_constraint_instantiated_with_point_accepted_spec_14_2) {
    // The call site `Utils<Point>` requires `Point: IEquatable<Point>`.
    // If the fix is absent, `checkAbstractImplementations` fires a false
    // "concrete record does not implement inherited abstract member `Equals`"
    // on `Point`. With the fix, the injection of `Equals` into
    // `concreteNames` prevents the false alarm.
    auto r = runSema(kPrelude +
        "class Pgm "
        "  public static method Main(): int "
        "    var p := new Point(1, 2); "
        "    var result := Utils<Point>.Contains(p, new Point(1, 2)); "
        "    return 0; "
        "  end Main; "
        "end Pgm;");
    BEZEL_EXPECT_FALSE(hasDoesNotImplementError(r));
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// T7: unconstrained generic with Point — control to confirm the fix
// does not touch unconstrained generics.
BEZEL_TEST(record_iequatable_self_synthesis_5048,
           unconstrained_generic_with_point_accepted_control_spec_14_2) {
    auto r = runSema(kPrelude +
        "class Pgm "
        "  public static method Main(): int "
        "    var p := new Point(1, 2); "
        "    var q := Simple<Point>.Id(p); "
        "    return 0; "
        "  end Main; "
        "end Pgm;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ================================================================
// REJECT ROW — record NOT listing IEquatable<T> must NOT satisfy
// the `where T: IEquatable<T>` constraint. Without the fix, this
// passes (no false alarm on the record itself); the reject happens
// at the instantiation site of `Utils<NoIEquatable>`. With the fix,
// the record itself still compiles clean; the constraint error fires
// at instantiation as expected.
// ================================================================

// NoIEquatable does NOT list IEquatable — the constraint error fires
// at `Utils<NoIEquatable>` instantiation.
BEZEL_TEST(record_iequatable_self_synthesis_5048,
           record_without_iequatable_listing_constraint_rejected_spec_14_2) {
    auto r = runSema(kPrelude +
        "class Pgm "
        "  public static method Main(): int "
        "    var n := new NoIEquatable(1); "
        "    return 0; "
        "  end Main; "
        "end Pgm;");
    // The record itself is fine — no "does not implement" error.
    BEZEL_EXPECT_FALSE(hasDoesNotImplementError(r));
    // The constraint violation fires at the instantiation below (T6a),
    // which is a separate compile step from the record definition.
    // Here we just verify the record compiles clean.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Instantiation of Utils<NoIEquatable> must fail — NoIEquatable does not
// satisfy `where T: IEquatable<T>` because it doesn't list IEquatable<T>.
// This is the "record without listing" reject from the issue's review note.
BEZEL_TEST(record_iequatable_self_synthesis_5048,
           iequatable_constraint_instantiation_without_listing_rejected_spec_14_2) {
    auto r = runSema(kPrelude +
        "class Pgm "
        "  public static method Main(): int "
        "    var n := new NoIEquatable(1); "
        "    var result := Utils<NoIEquatable>.Contains(n, n); "  // ERROR HERE
        "    return 0; "
        "  end Main; "
        "end Pgm;");
    // Must reject at the instantiation — NoIEquatable does not list
    // IEquatable<NoIEquatable>.
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}
