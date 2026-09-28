// fix #5097 (§11.2 / §12.5, v0.307): a class witnessing a generic interface's
// method obligation was matched by NAME + ARITY only, so a class declaring
// `IEquatable<Point>, IEquatable<int>` but implementing only
// `Equals(other: Point): bool` was wrongly accepted as satisfying BOTH
// obligations — `Equals(other: Point)` was picked as the (only) same-name,
// same-arity candidate for the `IEquatable<int>.Equals(int)` obligation too,
// with no check that `Point` and `int` actually match under the interface's
// substitution.
//
// §11.2 (v0.307): "A member matches an interface member when it has the
// same name ... and matching parameter and return types UNDER THE
// INTERFACE'S SUBSTITUTION." The per-parameter type-check loop in
// `Resolver::checkAbstractImplementations` (src/sema/TypeResolution_Checks.cpp)
// already existed and enforces exactly this — but it never fired for the
// BUILTIN synthesized interfaces (`IEquatable<T>`, `IComparable<T>`,
// `IEqualityComparer<T>`), because their method parameters are constructed ad
// hoc in `SemaContext::builtinInterfaceSym` (src/sema/SemaContext.hpp) and
// their parameter type AST nodes were never run through `setResolvedType` —
// unlike the parsed-from-source interface path, whose parameter types are
// resolved by the normal type-resolution pass. With `resolvedOf(ip->declType)`
// consistently null for a builtin-interface obligation, the parameter-type
// loop's `if (!it || !ct) continue;` guard silently skipped the check for
// EVERY obligation on IEquatable/IComparable/IEqualityComparer.
//
// THE FIX has two parts:
//  1. SemaContext.hpp: resolve each synthesized builtin-interface method
//     parameter's bare generic-parameter type (`Kind::Named` with
//     `declaration == nullptr`, mirroring `Resolver::resolveNamed`'s
//     unresolved-generic-parameter representation) via `setResolvedType`, so
//     the existing per-parameter check in TypeResolution_Checks.cpp actually
//     runs for builtin-interface obligations exactly as it already did for
//     user-declared generic interfaces (see repro row R_user_generic_reject
//     below, which already worked pre-fix and pins that the general
//     mechanism was never broken).
//  2. TypeResolution_Checks.cpp: the witness-selection walk previously took
//     the FIRST same-name/same-arity candidate unconditionally (a documented,
//     related-to-#4836 simplification). Once builtin-interface parameter
//     types resolve, that walk must also search every same-arity candidate
//     for one whose parameter types actually witness the CURRENT obligation
//     before falling back to the first-found candidate — otherwise a class
//     legitimately overloading `Equals` to witness two DIFFERENT
//     instantiations (`Equals(other: Point)` / `Equals(other: int)`) would be
//     falsely rejected merely for reusing the same method name twice.
//
// Every row compiles its own program (per the discipline in the sibling
// #5048/#5095 test) so a reject row can't be confused with a sibling row's
// unrelated failure.

#include <string>

#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCollect;

namespace {

bezel::test::SemaResult runSema(const std::string& src) {
    auto r = parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True when r carries the specific §11.2 "parameter types must match exactly
// under the interface's substitution" diagnostic this fix introduces/enables.
bool hasParamTypeMismatchError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(
                "parameter types must match exactly under the interface's "
                "substitution") != std::string::npos)
            return true;
    return false;
}

bool hasAnyError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) return true;
    return false;
}

// REJECT: the issue's exact repro. `Point` declares
// `IEquatable<Point>, IEquatable<int>` but implements only
// `Equals(other: Point): bool` — that single overload is name+arity
// compatible with BOTH obligations, but its parameter type only matches the
// `IEquatable<Point>` obligation. Must be rejected for the `IEquatable<int>`
// obligation with the specific §11.2 diagnostic (not merely "some error").
BEZEL_TEST(interface_witness_param_type_5097,
     wrong_typed_single_witness_for_two_iequatable_instantiations_rejected_spec_11_2) {
    const std::string src =
        "module M; "
        "record Point(X: int, Y: int): IEquatable<Point>, IEquatable<int> "
        "  public method Equals(other: Point): bool => X = other.X; "
        "  public override method GetHashCode(): int => X; "
        "end Point; "
        "class Pgm "
        "  public static method Main(): int return 0; end Main; "
        "end Pgm;";
    auto r = runSema(src);
    BEZEL_EXPECT_TRUE(hasParamTypeMismatchError(r));
}

// REJECT: the same shape for IComparable<T> — a class declares
// `IComparable<Box>, IComparable<int>` but implements only
// `CompareTo(other: Box): int`.
BEZEL_TEST(interface_witness_param_type_5097,
     wrong_typed_single_witness_for_two_icomparable_instantiations_rejected_spec_11_2) {
    const std::string src =
        "module M; "
        "class Box: IComparable<Box>, IComparable<int> "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public method CompareTo(other: Box): int => V - other.V; "
        "end Box; "
        "class Pgm "
        "  public static method Main(): int return 0; end Main; "
        "end Pgm;";
    auto r = runSema(src);
    BEZEL_EXPECT_TRUE(hasParamTypeMismatchError(r));
}

// ACCEPT: a single, correctly-typed witness for a single IEquatable<T>
// instantiation. Control proving the fix does not over-reject the ordinary,
// non-overloaded case.
BEZEL_TEST(interface_witness_param_type_5097,
     correctly_typed_single_witness_iequatable_accepted_spec_11_2) {
    const std::string src =
        "module M; "
        "record Point(X: int, Y: int): IEquatable<Point> "
        "  public method Equals(other: Point): bool => X = other.X; "
        "  public override method GetHashCode(): int => X; "
        "end Point; "
        "class Pgm "
        "  public static method Main(): int return 0; end Main; "
        "end Pgm;";
    auto r = runSema(src);
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// ACCEPT: a class provides TWO overloads of the same name/arity to
// correctly witness TWO different instantiations of the same generic
// interface (`Equals(other: Point)` for `IEquatable<Point>`,
// `Equals(other: int)` for `IEquatable<int>`). Pins Part 2 of the fix: the
// witness search must not confuse this legitimate overload pair for the
// wrong-typed single-witness case above.
BEZEL_TEST(interface_witness_param_type_5097,
     correctly_typed_overload_pair_for_two_iequatable_instantiations_accepted_spec_11_2) {
    const std::string src =
        "module M; "
        "class Point: IEquatable<Point>, IEquatable<int> "
        "  public X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "  public method Equals(other: Point): bool => X = other.X; "
        "  public method Equals(other: int): bool => X = other; "
        "  public override method GetHashCode(): int => X; "
        "end Point; "
        "class Pgm "
        "  public static method Main(): int return 0; end Main; "
        "end Pgm;";
    auto r = runSema(src);
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// ACCEPT control: a USER-DECLARED generic interface at a single
// instantiation with a correctly-typed witness. Pins that the general
// per-parameter-type mechanism (unaffected by this fix, since user-declared
// interface parameters were already resolved through the normal
// type-resolution pass) continues to work.
BEZEL_TEST(interface_witness_param_type_5097,
     user_declared_generic_interface_correct_witness_accepted_control) {
    const std::string src =
        "module M; "
        "interface IM<T> "
        "  method Get(x: T): T; "
        "end IM; "
        "class D: IM<string> "
        "  public method Get(x: string): string => x; "
        "end D; "
        "class Pgm "
        "  public static method Main(): int return 0; end Main; "
        "end Pgm;";
    auto r = runSema(src);
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// REJECT control: a USER-DECLARED generic interface at two instantiations
// with a wrong-typed sole witness. Pins that the general mechanism already
// rejected this shape pre-fix (proving this fix is specifically about the
// BUILTIN-interface parameter-resolution gap, not the general per-parameter
// check).
BEZEL_TEST(interface_witness_param_type_5097,
     user_declared_generic_interface_wrong_typed_witness_rejected_control) {
    const std::string src =
        "module M; "
        "interface IM<T> "
        "  method Get(x: T): T; "
        "end IM; "
        "class D: IM<string>, IM<int> "
        "  public method Get(x: string): string => x; "
        "end D; "
        "class Pgm "
        "  public static method Main(): int return 0; end Main; "
        "end Pgm;";
    auto r = runSema(src);
    BEZEL_EXPECT_TRUE(hasParamTypeMismatchError(r));
}

}  // namespace
