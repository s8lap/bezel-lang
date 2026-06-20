// fix #851 / §25.1: a compound assignment `lhs <op>= rhs` desugars to
// `lhs := lhs <op> rhs` using the binary operator's overload. When `lhs` is a
// user record/class whose `operator <op>` returns a type NOT assignable back to
// `lhs`, the implied assignment is a type error and must be rejected. Before the
// fix it was silently accepted, storing the foreign result reinterpreted as the
// LHS type (e.g. `operator +(Point, Point): int` made `p += q` store an `int`
// into a `Point` slot). These tests verify the reject fires for a result type
// that differs from the LHS, and that legitimate forms (same-type operator,
// primitive compound, user `+=` returning the LHS type) are NOT rejected.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// True iff a diagnostic mentions the §25.1 compound-assignment result-type rule.
bool hasCompoundResultError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("compound assignment") != std::string::npos &&
            d.message.find("result type differs") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §25.1: `operator +(Point, Point): int` — `p += q` desugars to `p := p + q`
// where `p + q : int` and `p : Point`, an unassignable type error. The bug08
// repro (was silently accepted, storing the int as a Point).
BEZEL_TEST(compound_assign_operator_result_851, plus_returns_int_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record Point(X: int, Y: int) "
        "  public static method operator +(a: Point, b: Point): int => a.X + b.X; "
        "end Point; "
        "class C public static method Main(): int "
        "  var p := new Point(3, 4); var q := new Point(5, 6); "
        "  p += q; return p.X; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasCompoundResultError(r));
}

// §25.1: a `-=` whose `operator -(Point, Point): bool` returns bool is rejected.
BEZEL_TEST(compound_assign_operator_result_851, minus_returns_bool_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record Point(X: int, Y: int) "
        "  public static method operator -(a: Point, b: Point): bool => a.X = b.X; "
        "end Point; "
        "class C public static method Main(): int "
        "  var p := new Point(3, 4); var q := new Point(5, 6); "
        "  p -= q; return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasCompoundResultError(r));
}

// §25.1: an `operator +` returning a DIFFERENT, unrelated record is rejected.
BEZEL_TEST(compound_assign_operator_result_851, plus_returns_other_record_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record Other(Z: int); "
        "record Point(X: int, Y: int) "
        "  public static method operator +(a: Point, b: Point): Other => new Other(a.X); "
        "end Point; "
        "class C public static method Main(): int "
        "  var p := new Point(3, 4); var q := new Point(5, 6); "
        "  p += q; return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasCompoundResultError(r));
}

// Accepted: an `operator +` that returns the LHS type — the desugared
// `p := p + q` is well-typed.
BEZEL_TEST(compound_assign_operator_result_851, plus_returns_same_type_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record Point(X: int, Y: int) "
        "  public static method operator +(a: Point, b: Point): Point => "
        "    new Point(a.X + b.X, a.Y + b.Y); "
        "end Point; "
        "class C public static method Main(): int "
        "  var p := new Point(3, 4); var q := new Point(5, 6); "
        "  p += q; return p.X; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasCompoundResultError(r));
}

// Accepted: a primitive compound assignment (no user operator overload).
BEZEL_TEST(compound_assign_operator_result_851, primitive_compound_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method Main(): int "
        "  var x := 5; x += 3; return x; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasCompoundResultError(r));
}
