// Spec-derived sema tests for fix #864 — two generic constraint gaps:
//
// PART 1 (§17.11): `default(T)` under `where T: class` (or a type-bound
//   constraint) is a compile error — the constraint guarantees T is
//   non-nullable, so the null value `default(T)` would produce does not fit
//   T's contract.  `default(T?)` (nullable) must remain accepted, as must
//   `default(T) where T: value` and unconstrained `default(T)`.
//
// PART 2 (§14.2): `where T: new()` is not checked at a METHOD CALL site
//   when explicit type arguments are supplied (e.g. `Make<RequiresArg>()`
//   where RequiresArg has only a parameterised constructor, violating new()).
//   Classes with an accessible parameterless constructor must still be accepted.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// PART 1 — §17.11 / #864: `default(T)` under `where T: class` → error
// ---------------------------------------------------------------------------

// §17.11 (#864 part 1): `default(T)` inside a method constrained `where T:
// class` is rejected — T is guaranteed non-null, so default(T)=null is ill-typed.
BEZEL_TEST(generic_default_new_constraint_864, part1_default_T_where_class_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo public constructor() end constructor; destructor end destructor; end Foo; "
        "static class Utils "
        "  public static method GetNull<T>(): T where T: class "
        "    return default(T); "
        "  end GetNull; "
        "end Utils; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, "default(T)") && hasError(r, "17.11"));
}

// §17.11 (#864 part 1): `default(T)` under a type-bound constraint (`where
// T: Foo`) also guarantees non-null T → same compile error.
BEZEL_TEST(generic_default_new_constraint_864, part1_default_T_type_bound_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo public constructor() end constructor; destructor end destructor; end Foo; "
        "static class Utils "
        "  public static method GetNull<T>(): T? where T: Foo "
        "    return default(T); "
        "  end GetNull; "
        "end Utils; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, "default(T)") && hasError(r, "17.11"));
}

// §17.11 (#864 part 1 ACCEPT): `default(T?)` (nullable T) under
// `where T: class` MUST remain accepted — `T?` allows null.
BEZEL_TEST(generic_default_new_constraint_864, part1_default_Tnullable_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "static class Utils "
        "  public static method GetNull<T>(): T? where T: class "
        "    return default(T?); "
        "  end GetNull; "
        "end Utils; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(hasError(r, "17.11"));
    // No constraint error either — T? is the nullable counterpart, which is fine.
    BEZEL_EXPECT_FALSE(hasError(r, "does not satisfy"));
}

// §17.11 (#864 part 1 ACCEPT): `default(T) where T: value` is accepted —
// value types always have a well-defined zero value.
BEZEL_TEST(generic_default_new_constraint_864, part1_default_T_value_constraint_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "static class Utils "
        "  public static method GetZero<T>(): T where T: value "
        "    return default(T); "
        "  end GetZero; "
        "end Utils; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(hasError(r, "17.11"));
}

// §17.11 (#864 part 1 ACCEPT): unconstrained `default(T)` is accepted —
// T may be a value type (a common use-case for generic zero-initialisation).
BEZEL_TEST(generic_default_new_constraint_864, part1_default_T_unconstrained_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "static class Utils "
        "  public static method GetDefault<T>(v: T): T "
        "    return default(T); "
        "  end GetDefault; "
        "end Utils; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(hasError(r, "17.11"));
}

// ---------------------------------------------------------------------------
// PART 2 — §14.2 / #864: `where T: new()` checked at METHOD call sites
// ---------------------------------------------------------------------------

// §14.2 (#864 part 2): calling `Make<RequiresArg>()` where RequiresArg
// has only a parameterised constructor violates `where T: new()` → error.
BEZEL_TEST(generic_default_new_constraint_864, part2_requires_arg_violates_new) {
    auto r = parseSemaAll(
        "module M; "
        "class RequiresArg "
        "  public _n: int := 0; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  destructor end destructor; "
        "end RequiresArg; "
        "static class Utils "
        "  public static method Make<T>(): int where T: class, new() "
        "    using var t := new T(); return 1; "
        "  end Make; "
        "end Utils; "
        "class C public static method Main(): int "
        "  return Utils.Make<RequiresArg>(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, "new()") && hasError(r, "14.2"));
}

// §14.2 (#864 part 2 ACCEPT): `Make<Foo>()` where Foo has a parameterless
// constructor MUST be accepted — Foo satisfies `where T: new()`.
BEZEL_TEST(generic_default_new_constraint_864, part2_parameterless_ctor_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Foo; "
        "static class Utils "
        "  public static method Make<T>(): int where T: class, new() "
        "    using var t := new T(); return 1; "
        "  end Make; "
        "end Utils; "
        "class C public static method Main(): int "
        "  return Utils.Make<Foo>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasError(r, "new()"));
}

// §14.2 (#864 part 2 ACCEPT): `Make<NoCtorClass>()` where NoCtorClass has
// NO explicit constructor (implicit default ctor) must be accepted.
BEZEL_TEST(generic_default_new_constraint_864, part2_implicit_ctor_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class NoCtorClass "
        "  public n: int := 0; "
        "  destructor end destructor; "
        "end NoCtorClass; "
        "static class Utils "
        "  public static method Make<T>(): int where T: class, new() "
        "    using var t := new T(); return 1; "
        "  end Make; "
        "end Utils; "
        "class C public static method Main(): int "
        "  return Utils.Make<NoCtorClass>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasError(r, "new()"));
}

// ===========================================================================
// §17.11 (#951): default(T) for a CONCRETE non-nullable reference type — a
// class, interface, `string`, or function type — is a compile error (no
// well-defined zero value). `default(T?)` and value types stay accepted.
// ===========================================================================

// REJECT: default(string) — string is a non-nullable runtime-managed reference.
BEZEL_TEST(generic_default_new_constraint_864, default_string_rejected_951) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var s: string? := default(string); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, "default(T)") && hasError(r, "17.11"));
}

// REJECT: default(ClassType) — a user class is a non-nullable reference type.
BEZEL_TEST(generic_default_new_constraint_864, default_class_rejected_951) {
    auto r = parseSemaAll(
        "module M; class Cell destructor end destructor; end Cell; "
        "class C public static method Main(): int "
        "  var c: Cell? := default(Cell); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, "default(T)") && hasError(r, "17.11"));
}

// ACCEPT: default(T?) of the nullable counterpart, and value-type defaults.
BEZEL_TEST(generic_default_new_constraint_864, default_nullable_and_value_accepted_951) {
    auto r = parseSemaAll(
        "module M; class Cell destructor end destructor; end Cell; "
        "class C public static method Main(): int "
        "  var c: Cell? := default(Cell?); "
        "  var s: string? := default(string?); "
        "  var i: int := default(int); var b: bool := default(bool); "
        "  return i + (1 if b else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
