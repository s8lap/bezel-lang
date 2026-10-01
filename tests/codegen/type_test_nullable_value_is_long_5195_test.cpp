// Issue #5195: nullable value type is/as against a different concrete value type
// Spec §12.7/§19.3 (v0.312/#5182): nullable value operand `p is T` where T is
// a different value type must be false (not hasValue). The hasValue branch
// was returning true for any non-null value.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

int run(const char* src) {
    JitResult r = runAot(src);
    if (!r.ran) return -1;
    return r.exitCode;
}

}  // namespace

// §12.7 "Value-typed operands" (v0.313): the nullable form of a value type is
// a value-typed operand, and a concrete one tested against a value type other
// than its own or its own nullable form is a COMPILE ERROR (#5175), so the
// concrete `int? is long` shape this issue first probed is rejected, not
// decided false at runtime. The #5195 codegen fix (a nullable value `is T`
// compares the underlying type, not hasValue) stays reachable through a
// type-parameter carrier instantiated at a nullable value type (below).
static bool rejectedConcreteNullable(const char* src) {
    JitResult r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("a concrete value-typed operand can only be tested") !=
            std::string::npos)
            return true;
    }
    return false;
}

BEZEL_TEST(type_test_nullable_value_is_long_5195, nullable_int_is_long_rejected_spec_12_7) {
    BEZEL_EXPECT_TRUE(rejectedConcreteNullable(
        "module M; class C "
        "  public static method Main(): int "
        "    var n: int? := 5; "
        "    if n is long then return 99; end; "
        "    return 3; "
        "  end Main; "
        "end C;"));
}

BEZEL_TEST(type_test_nullable_value_is_long_5195, nullable_int_is_not_long_rejected_spec_12_7) {
    BEZEL_EXPECT_TRUE(rejectedConcreteNullable(
        "module M; class C "
        "  public static method Main(): int "
        "    var n: int? := 5; "
        "    if n is not long then return 1; end; "
        "    return 99; "
        "  end Main; "
        "end C;"));
}

// §12.7 (v0.312/#5170 correction, #5195): `int? is int` with non-null value
// should be true (same underlying type, hasValue=true).
// This is the correct behavior that was already working.
// Expected: 1.
BEZEL_TEST(type_test_nullable_value_is_long_5195, nullable_int_is_int_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var n: int? := 5; "
        "    if n is int then return 1; end; "
        "    return 99; "
        "  end Main; "
        "end C;"), 1);
}

// §12.7 (v0.312/#5170 correction, #5195): `int? is int` with null value
// should be false (null has no type).
// Expected: 3.
BEZEL_TEST(type_test_nullable_value_is_long_5195, nullable_null_is_int_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var n: int? := null; "
        "    if n is int then return 99; end; "
        "    return 3; "
        "  end Main; "
        "end C;"), 3);
}

// §12.7 (v0.312/#5170 correction, #5195): generic nullable carrier `v: T?` at T=int
// should also correctly use the underlying type comparison.
// `F<int?>` with v is long → false. Expected: 3.
BEZEL_TEST(type_test_nullable_value_is_long_5195, generic_nullable_carrier_is_long_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class F<T> "
        "  public v: T; "
        "  public constructor(x: T) v := x; end; "
        "  public method Check(): int "
        "    if v is long then return 1; end; "
        "    return 3; "
        "  end Check; "
        "end F; "
        "public static class P "
        "  public static method Main(): int "
        "    return new F<int?>(5).Check(); "
        "  end Main; "
        "end P;"), 3);
}
