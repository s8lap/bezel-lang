// test #5086 / spec §14.2 (v0.307): fix ea0a909e cited this regression
// test ("generic_record_iequatable_self_constraint_5086 (4 tests)") but
// never committed it. This file supplies it.
//
// BUG: a generic record listing `IEquatable<Self<T>>` (e.g.
// `record Pair<T>(A: T, B: T): IEquatable<Pair<T>>`) was falsely rejected
// at a `where T: IEquatable<T>` constraint site once instantiated (e.g.
// `Gen<Pair<string>>`). `Resolver::argSatisfiesTypeBound`'s BFS walks
// `baseTypeRefs` and, when the bound's generic arg is a bare parameter
// marker, substitutes it with the full candidate arg (`Pair<string>`) —
// but did not also substitute the nested `T` inside the record's own
// `IEquatable<Pair<T>>` base-type-ref (`tRef->genericArgs[j]`), so
// `Pair<string>` was compared against unsubstituted `Pair<T>` and the
// comparison spuriously failed.
//
// THE FIX (src/sema/TypeResolution_Checks.cpp, TypeResolution.hpp/_impl.hpp):
// `argSatisfiesTypeBound` takes an optional `SemaContext*`; when the bound
// arg is a bare parameter marker, it now substitutes the candidate's own
// type-parameter bindings into the base's generic arg before the
// structural `typeRefEqual` comparison, so `Pair<string>` is compared
// against `Pair<string>`, not `Pair<T>`.
//
// FAIL-WITHOUT (this file, T1): the issue's literal repro rejects with
// "type argument `Pair` does not satisfy the constraint `where T:
// IEquatable` (§14.2)" pre-fix; it compiles and runs (exit 1) with the fix.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// T1: the issue's verbatim repro (#5086). `Pair<T>: IEquatable<Pair<T>>`
// instantiated as `Pair<string>` must satisfy `where T: IEquatable<T>` at
// `Gen<Pair<string>>`, per §14.1/§14.2: substituting `T := string` into
// `Pair<T>`'s inheritance clause gives `IEquatable<Pair<string>>`, exactly
// the bound `IEquatable<T>` at `T := Pair<string>`.
BEZEL_TEST(generic_record_iequatable_self_constraint_5086,
           literal_repro_pair_string_accepted_spec_14_2) {
    auto r = runAot(
        "module M; "
        "record Pair<T>(A: T, B: T): IEquatable<Pair<T>>; "
        "class App "
        "  public static method Gen<T>(a: T, b: T): bool where T: IEquatable<T> "
        "    => a.Equals(b); "
        "  public static method Main(): int "
        "    var p := new Pair<string>(\"x\", \"y\"); "
        "    var q := new Pair<string>(\"x\", \"y\"); "
        "    if Gen<Pair<string>>(p, q) then return 1; end if; "
        "    return 0; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// T2: same shape, but the generic argument to the record is `int` instead
// of `string` — confirms the fix's inner substitution is generic over the
// candidate's own type-parameter binding, not special-cased to `string`.
BEZEL_TEST(generic_record_iequatable_self_constraint_5086,
           pair_int_accepted_control_spec_14_2) {
    auto r = runAot(
        "module M; "
        "record Pair<T>(A: T, B: T): IEquatable<Pair<T>>; "
        "class App "
        "  public static method Gen<T>(a: T, b: T): bool where T: IEquatable<T> "
        "    => a.Equals(b); "
        "  public static method Main(): int "
        "    var p := new Pair<int>(1, 2); "
        "    var q := new Pair<int>(1, 2); "
        "    if Gen<Pair<int>>(p, q) then return 1; end if; "
        "    return 0; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// T3: the fields differ so `Equals` is false, and the constraint-satisfied
// program must correctly RUN the record's synthesized equality (stdlib
// §2.9) rather than degrade to a vacuous true/false — pins the actual
// runtime value, not just "compiled and ran".
BEZEL_TEST(generic_record_iequatable_self_constraint_5086,
           pair_string_unequal_fields_runs_correct_value_spec_14_2) {
    auto r = runAot(
        "module M; "
        "record Pair<T>(A: T, B: T): IEquatable<Pair<T>>; "
        "class App "
        "  public static method Gen<T>(a: T, b: T): bool where T: IEquatable<T> "
        "    => a.Equals(b); "
        "  public static method Main(): int "
        "    var p := new Pair<string>(\"x\", \"y\"); "
        "    var q := new Pair<string>(\"x\", \"z\"); "
        "    if Gen<Pair<string>>(p, q) then return 1; end if; "
        "    return 0; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// T5: same nested-generic-self-constraint shape as T1, but reached through
// the OTHER argSatisfiesTypeBound call site: a generic type-ALIAS's own
// `where` clause (TypeResolution_Names.cpp ~3766, fix #4464's path), not a
// generic-method call-site check (ExprTypecheck_Calls.cpp). `Maker5086<T>`
// aliases `Box<T>` with `where T: IEquatable<T>`; instantiating it with
// `Pair<string>` requires the alias-resolution walk to substitute the
// nested `T` inside `Pair<T>`'s own `IEquatable<Pair<T>>` base-type-ref —
// exactly the fix's substitution, but exercised via `resolveNamed`'s
// alias-constraint check rather than a call-site constraint check. Before
// this fix-forward, this call site passed no SemaContext*, so it crashed
// (nullptr deref in the nested-substitution branch) rather than merely
// rejecting.
BEZEL_TEST(generic_record_iequatable_self_constraint_5086,
           alias_where_clause_nested_generic_accepted_spec_14_2) {
    auto r = runAot(
        "module M; "
        "record Pair<T>(A: T, B: T): IEquatable<Pair<T>>; "
        "class Box<T> "
        "  public Value: T; "
        "  public constructor(v: T) Value := v; end constructor; "
        "end Box; "
        "type Maker5086<T> := Box<T> where T : IEquatable<T>; "
        "class App "
        "  public static method Main(): int "
        "    var p := new Pair<string>(\"x\", \"y\"); "
        "    var b: Maker5086<Pair<string>> := new Box<Pair<string>>(p); "
        "    return 1; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// T4: the non-generic control from the issue's Notes — a plain
// `record Point(...): IEquatable<Point>` at `Gen<Point>` was already
// accepted pre-fix (the bug is specific to the NESTED generic-arg case).
// Pinned here so a future change to this BFS cannot silently regress the
// simple, already-working case while "fixing" the nested one.
BEZEL_TEST(generic_record_iequatable_self_constraint_5086,
           non_generic_point_accepted_control_spec_14_2) {
    auto r = runAot(
        "module M; "
        "record Point(X: int, Y: int): IEquatable<Point>; "
        "class App "
        "  public static method Gen<T>(a: T, b: T): bool where T: IEquatable<T> "
        "    => a.Equals(b); "
        "  public static method Main(): int "
        "    var p := new Point(1, 2); "
        "    var q := new Point(1, 2); "
        "    if Gen<Point>(p, q) then return 1; end if; "
        "    return 0; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
