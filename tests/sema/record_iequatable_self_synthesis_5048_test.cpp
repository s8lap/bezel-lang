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
//
// fix #5095 (§9.1 / §12.5 / §2.3, v0.307) narrowed that injection: the
// synthesized witness satisfies ONLY the auto-imported `IEquatable<T>` at
// T = exactly Self, so a user interface shadowing the name, a non-Self type
// argument (`Pair<T>: IEquatable<Pair<int>>`, `IEquatable<Point?>`) and a second
// `IEquatable<U>` entry all keep the §12.5 reject (rows R1-R4 below). The
// injection is now decided after the BFS (see the comment above `builtinIEq`).
//
// EVERY ROW COMPILES ITS OWN PROGRAM (#5095 review): the rows originally shared
// one prelude declaring every record, so each accept row accepted or rejected
// ALL of them at once and a reject row could not be told apart from a sibling's
// failure. Each program now declares only what its row is about. The cross-
// module rows are tests/module/xmod_record_iequatable_self_5095_test.cpp.

#include <string>

#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCollect;

namespace {

const std::string kMain =
    "class Pgm "
    "  public static method Main(): int return 0; end Main; "
    "end Pgm;";

// `where T: IEquatable<T>` generic used by the constraint rows (T6, T8, T9).
const std::string kUtils =
    "static class Utils<T> where T: IEquatable<T> "
    "  public static method Contains(a: T, b: T): bool => a.Equals(b); "
    "end Utils; ";

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

// True when r carries the §12.5 reject for `record_` not implementing the
// inherited abstract `Equals` — the exact pre-#5048 diagnostic #5095 restores.
bool hasEqualsNotImplemented(const bezel::test::SemaResult& r,
                             const std::string& record) {
    const std::string needle = "concrete record `" + record +
        "` does not implement inherited abstract member `Equals` (§12.5)";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
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
    auto r = runSema(
        "module M; "
        "record Point(X: int, Y: int): IEquatable<Point>; " + kMain);
    BEZEL_EXPECT_FALSE(hasDoesNotImplementError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// T2: nested record — the inner type satisfies the constraint via synthesis.
BEZEL_TEST(record_iequatable_self_synthesis_5048,
           nested_record_lists_iequatable_self_accepted_spec_14_2_rule_2) {
    auto r = runSema(
        "module M; "
        "record Outer(Value: int): IEquatable<Outer>; "
        "record Inner(Data: string): IEquatable<Inner>; " + kMain);
    BEZEL_EXPECT_FALSE(hasDoesNotImplementError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// T3: body record with auto-property (isAutoEqualityBearingBodyRecord).
BEZEL_TEST(record_iequatable_self_synthesis_5048,
           body_record_auto_property_lists_iequatable_self_accepted_spec_14_2_rule_2) {
    auto r = runSema(
        "module M; "
        "record Label: IEquatable<Label> "
        "  public property Name: string; "
        "  public constructor(n: string) Name := n; end constructor; "
        "end Label; " + kMain);
    BEZEL_EXPECT_FALSE(hasDoesNotImplementError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// T4: body record with explicit Equals — the explicit member IS the witness.
BEZEL_TEST(record_iequatable_self_synthesis_5048,
           body_record_explicit_equals_lists_iequatable_self_accepted_spec_14_2_rule_2) {
    auto r = runSema(
        "module M; "
        "record Person: IEquatable<Person> "
        "  public Name: string; "
        "  public constructor(n: string) Name := n; end constructor; "
        "  public method Equals(other: Person): bool => Name = other.Name; "
        "  public method GetHashCode(): int => Name.GetHashCode(); "
        "end Person; " + kMain);
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
    auto r = runSema(
        "module M; "
        "record Point(X: int, Y: int): IEquatable<Point>; " + kUtils +
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
    auto r = runSema(
        "module M; "
        "record Point(X: int, Y: int): IEquatable<Point>; "
        "static class Simple<T> "
        "  public static method Id(x: T): T return x; end Id; "
        "end Simple; "
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
    auto r = runSema(
        "module M; "
        "record NoIEquatable(A: int); " + kUtils +
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
    auto r = runSema(
        "module M; "
        "record NoIEquatable(A: int); " + kUtils +
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

// ================================================================
// #5095 ROWS (§9.1 / §12.5 / §2.3, v0.307) — the synthesized `Equals(T)`
// witnesses the auto-imported `IEquatable<T>` at EXACTLY Self, once.
// ================================================================

// A8 (accept, the positive side of R1): a GENERIC record listing
// `IEquatable<Pair<T>>` — Self is the record at its own generic parameters.
BEZEL_TEST(record_iequatable_self_synthesis_5048,
           a8_generic_record_lists_iequatable_self_at_own_params_accepted_spec_9_1) {
    auto r = runSema(
        "module M; "
        "record Pair<T>(A: T, B: T): IEquatable<Pair<T>>; "
        // (The `where T: IEquatable<T>` instantiation at `Pair<int>` is
        // #5086, a separate pre-existing §14.2 false reject; this row pins
        // only the declaration and a direct call.)
        "class Pgm "
        "  public static method Main(): int "
        "    var ok := new Pair<int>(1, 2).Equals(new Pair<int>(1, 2)); "
        "    return 0; "
        "  end Main; "
        "end Pgm;");
    BEZEL_EXPECT_FALSE(hasDoesNotImplementError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// R1 (#5095 shape b, the issue's s1): `Pair<T>: IEquatable<Pair<int>>` is not
// Self — the head declaration matches but the type arguments do not. The
// synthesized `Equals(Pair<T>)` does not witness `Equals(Pair<int>)`; pre-fix
// this compiled and SEGV'd in `bz$boxieq$Pair$Pair_string` (type confusion).
BEZEL_TEST(record_iequatable_self_synthesis_5048,
           r1_generic_record_closed_iequatable_instantiation_rejected_spec_12_5) {
    auto r = runSema(
        "module M; "
        "record Pair<T>(A: T, B: T): IEquatable<Pair<int>>; " + kMain);
    BEZEL_EXPECT_TRUE(hasEqualsNotImplemented(r, "Pair"));
}

// R2 (#5095 shape a, the issue's s3): a USER `interface IEquatable<T>` shadows
// the auto-imported name (§2.3 permits it; #3139's precedent). It is a
// different interface, the synthesized witness is not its implementation, and
// the §12.5 reject stands. Pre-fix: compiled, then SEGV via a null itable slot.
BEZEL_TEST(record_iequatable_self_synthesis_5048,
           r2_user_interface_shadowing_iequatable_rejected_spec_2_3_12_5) {
    auto r = runSema(
        "module M; "
        "interface IEquatable<T> "
        "  method Equals(other: T): bool; "
        "end IEquatable; "
        "record Point(X: int, Y: int): IEquatable<Point>; " + kMain);
    BEZEL_EXPECT_TRUE(hasEqualsNotImplemented(r, "Point"));
}

// R3 (#5095 shape c, the issue's s2): a duplicate listing. The Self entry is
// witnessed by the synthesized `Equals(Point)`; the `IEquatable<int>` entry is
// a separate obligation with no `Equals(int)` anywhere, so the record is
// rejected. Pre-fix: compiled and dispatched an int into `Equals(Point)`.
BEZEL_TEST(record_iequatable_self_synthesis_5048,
           r3_duplicate_iequatable_listing_non_self_entry_rejected_spec_12_5) {
    auto r = runSema(
        "module M; "
        "record Point(X: int, Y: int): IEquatable<Point>, IEquatable<int>; " +
        kMain);
    BEZEL_EXPECT_TRUE(hasEqualsNotImplemented(r, "Point"));
}

// R4 (#5095 shape b, nullable spelling): `IEquatable<Point?>` names the same
// declaration as Self but is a different type (§8.3 `T?`); full type
// comparison, not the head, decides Self-ness.
BEZEL_TEST(record_iequatable_self_synthesis_5048,
           r4_nullable_self_iequatable_argument_rejected_spec_12_5) {
    auto r = runSema(
        "module M; "
        "record Point(X: int, Y: int): IEquatable<Point?>; " + kMain);
    BEZEL_EXPECT_TRUE(hasEqualsNotImplemented(r, "Point"));
}
