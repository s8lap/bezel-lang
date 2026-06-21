// Codegen — issue #863 (part 1): calling `a.Equals(b)` on a generic value `a` of
// type `T` bound `where T: IEquatable<T>` returned false for every primitive,
// because the path that resolves a bare type-parameter receiver to its concrete
// PRIMITIVE type (the #730 substitution) had a `CompareTo` handler but no
// `Equals` handler — so the call fell through to an interface dispatch that
// returned 0. Added a primitive `Equals` handler (value equality; FP ordered eq)
// alongside the existing `CompareTo` one.
//
// stdlib §9.1–§9.5 (IEquatable<T> for numeric / char / bool). The record/class
// half of #863 (dispatch through the BUILTIN IComparable/IEquatable constraint to
// a user record's method) is a separate sema+codegen gap, tracked separately.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
const char* kEq =
    "static class U "
    "  public static method Eq<T>(a: T, b: T): int where T: IEquatable<T> "
    "    return (1 if a.Equals(b) else 0); end Eq; "
    "end U; ";
}  // namespace

// int via IEquatable<T> constraint — equal and not-equal.
BEZEL_TEST(primitive_equals_constraint_863, int_equals_via_constraint) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kEq +
        "class C public static method Main(): int "
        "  return U.Eq(7, 7) * 10 + U.Eq(7, 9); "  // 1*10 + 0 = 10
        "end Main; end C;"), 10);
}

// char and long instantiations also dispatch.
BEZEL_TEST(primitive_equals_constraint_863, char_and_long_equals_via_constraint) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kEq +
        "class C public static method Main(): int "
        "  var c := U.Eq('x', 'x'); "   // 1
        "  var d := U.Eq(5L, 6L); "      // 0
        "  return c * 10 + d; "          // 10
        "end Main; end C;"), 10);
}

// the existing CompareTo path (the #730 sibling) must still work.
BEZEL_TEST(primitive_equals_constraint_863, compareto_still_dispatches) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "static class U "
        "  public static method Cmp<T>(a: T, b: T): int where T: IComparable<T> "
        "    return (1 if a.CompareTo(b) < 0 else 0); end Cmp; "
        "end U; "
        "class C public static method Main(): int "
        "  return U.Cmp(3, 9); "  // 3 < 9 → 1
        "end Main; end C;"), 1);
}
