// Spec-derived tests for issue #576: IComparable<T> and IEquatable<T>
// must resolve as type names (per stdlib §2.8, §2.9 and spec §2.3
// auto-import list). They are auto-imported with System; a class or record
// declaring `: IComparable<T>` or `: IEquatable<T>` must NOT produce
// "unresolved type name".

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some diagnostic contains the given substring.
bool hasDiagContaining(const bezel::test::SemaResult& r,
                       const std::string& sub) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(sub) != std::string::npos) return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// IComparable<T> — stdlib §2.8, spec §2.3 auto-import list
// ---------------------------------------------------------------------------

// §2.8 / §2.3: A class implementing IComparable<Self> with CompareTo must
// resolve the base-type name without error.  This is the canonical repro from
// issue #576.
BEZEL_TEST(sema_576_icomparable_iequatable,
           class_implements_icomparable_resolves_2_8) {
    auto r = parseSemaAll(
        "module Test; "
        "class MyInt : IComparable<MyInt> "
        "  private _val: int; "
        "  public constructor(v: int) _val := v; end constructor; "
        "  public method CompareTo(other: MyInt): int "
        "    if _val < other._val then return -1; end; "
        "    if _val > other._val then return 1; end; "
        "    return 0; "
        "  end CompareTo; "
        "end MyInt;");
    BEZEL_EXPECT_FALSE(hasDiagContaining(r, "unresolved type name"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §2.8: IComparable<T> as a field type resolves (arity 1).
BEZEL_TEST(sema_576_icomparable_iequatable,
           icomparable_as_field_type_resolves_2_8) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public _comp: IComparable<int>?; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasDiagContaining(r, "unresolved type name"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §2.8: IComparable<T> as a method parameter type resolves.
BEZEL_TEST(sema_576_icomparable_iequatable,
           icomparable_as_param_type_resolves_2_8) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method Foo(x: IComparable<int>?): int "
        "    return 0; "
        "  end Foo; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasDiagContaining(r, "unresolved type name"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §2.8: IComparable<T> with wrong arity (0 args) must diagnose.
BEZEL_TEST(sema_576_icomparable_iequatable,
           icomparable_wrong_arity_0_diagnoses_2_8) {
    // IComparable is arity 1; bare IComparable (no type arg) must error.
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public _x: IComparable?; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §2.8: IComparable<T,U> (arity 2) must diagnose.
BEZEL_TEST(sema_576_icomparable_iequatable,
           icomparable_wrong_arity_2_diagnoses_2_8) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public _x: IComparable<int, int>?; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// IEquatable<T> — stdlib §2.9, spec §2.3 auto-import list
// ---------------------------------------------------------------------------

// §2.9 / §2.3: A class implementing IEquatable<Self> with Equals must
// resolve the base-type name without error.
BEZEL_TEST(sema_576_icomparable_iequatable,
           class_implements_iequatable_resolves_2_9) {
    auto r = parseSemaAll(
        "module Test; "
        "class Point : IEquatable<Point> "
        "  private _x: int; "
        "  private _y: int; "
        "  public constructor(x: int, y: int) _x := x; _y := y; end constructor; "
        "  public method Equals(other: Point): bool "
        "    return _x = other._x and _y = other._y; "
        "  end Equals; "
        "end Point;");
    BEZEL_EXPECT_FALSE(hasDiagContaining(r, "unresolved type name"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §2.9: IEquatable<T> as a field type resolves.
BEZEL_TEST(sema_576_icomparable_iequatable,
           iequatable_as_field_type_resolves_2_9) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public _eq: IEquatable<int>?; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasDiagContaining(r, "unresolved type name"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §2.9: IEquatable<T> as method return type resolves.
BEZEL_TEST(sema_576_icomparable_iequatable,
           iequatable_as_return_type_resolves_2_9) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method GetEq(): IEquatable<int>? "
        "    return null; "
        "  end GetEq; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasDiagContaining(r, "unresolved type name"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §2.9: IEquatable<T> with wrong arity (0 args) must diagnose.
BEZEL_TEST(sema_576_icomparable_iequatable,
           iequatable_wrong_arity_0_diagnoses_2_9) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public _x: IEquatable?; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §2.9: IEquatable<T,U> (arity 2) must diagnose.
BEZEL_TEST(sema_576_icomparable_iequatable,
           iequatable_wrong_arity_2_diagnoses_2_9) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public _x: IEquatable<int, int>?; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Record implementing IComparable<T> — stdlib §2.8, spec §9.2
// ---------------------------------------------------------------------------

// §2.8 / §9.2: Records may implement IComparable<T> (it is not IDisposable).
// Spec §9.6 example: record Point(X: int, Y: int): IComparable<Point>.
BEZEL_TEST(sema_576_icomparable_iequatable,
           record_implements_icomparable_resolves_2_8_9_2) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int): IComparable<Point> "
        "  public method CompareTo(other: Point): int "
        "    return X - other.X; "
        "  end CompareTo; "
        "end Point;");
    BEZEL_EXPECT_FALSE(hasDiagContaining(r, "unresolved type name"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §2.8 / §14.2: `where T: IComparable<T>` generic constraint resolves.
BEZEL_TEST(sema_576_icomparable_iequatable,
           where_icomparable_constraint_resolves_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class Sorter<T> where T: IComparable<T> "
        "  public method Compare(a: T, b: T): int "
        "    return a.CompareTo(b); "
        "  end Compare; "
        "end Sorter;");
    BEZEL_EXPECT_FALSE(hasDiagContaining(r, "unresolved type name"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §2.9 / §14.2: `where T: IEquatable<T>` generic constraint resolves.
BEZEL_TEST(sema_576_icomparable_iequatable,
           where_iequatable_constraint_resolves_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class Container<T> where T: IEquatable<T> "
        "  private _val: T?; "
        "  public method Contains(x: T): bool "
        "    if _val = null then return false; end; "
        "    return _val!.Equals(x); "
        "  end Contains; "
        "end Container;");
    BEZEL_EXPECT_FALSE(hasDiagContaining(r, "unresolved type name"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
