// Issue #5191: per-instantiation type-parameter pattern matching
// Spec §19.3/§12.7 (v0.311/#5170): a bare type-parameter pattern `T` in a
// generic body is decided per instantiation — match only when the runtime
// type of the scrutinee equals T.

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

// §19.3/§12.7 (v0.311/#5170): bare type-param pattern `case p of T` at B<int>
// T=int, p is int → match (expect 1)
BEZEL_TEST(type_test_type_param_per_instantiation_5191, bare_type_param_pattern_B_int_match) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B<T> "
        "  public static method K(p: int): int => "
        "    (case p of T => 1, var x: T => 2, _ => 3); "
        "end B; "
        "public static class P "
        "  public static method Main(): int => B<int>.K(5); "
        "end P;"), 1);
}

// §19.3/§12.7 (v0.311/#5170): bare type-param pattern `case p of T` at B<long>
// T=long, p is int → no match (expect 3)
BEZEL_TEST(type_test_type_param_per_instantiation_5191, bare_type_param_pattern_B_long_fallback) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B<T> "
        "  public static method K(p: int): int => "
        "    (case p of T => 1, var x: T => 2, _ => 3); "
        "end B; "
        "public static class P "
        "  public static method Main(): int => B<long>.K(5); "
        "end P;"), 3);
}

// §19.3/§12.7 (v0.311/#5170) guard: generic carrier `v is long` at T=int
// stays admitted per instantiation — not rejected. Decides false (expect 3).
BEZEL_TEST(type_test_type_param_per_instantiation_5191, generic_carrier_is_long_at_int_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class E<T> "
        "  public v: T; "
        "  public constructor(x: T) v := x; end; "
        "  public method Check(): int "
        "    if v is long then return 1; end; "
        "    return 3; "
        "  end Check; "
        "end E; "
        "public static class P "
        "  public static method Main(): int "
        "    return new E<int>(5).Check(); "
        "  end Main; "
        "end P;"), 3);
}

// §19.3/§12.7 (v0.311/#5170): generic carrier `v is long` at T=long → true (expect 1)
BEZEL_TEST(type_test_type_param_per_instantiation_5191, generic_carrier_is_long_at_long_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class E<T> "
        "  public v: T; "
        "  public constructor(x: T) v := x; end; "
        "  public method Check(): int "
        "    if v is long then return 1; end; "
        "    return 3; "
        "  end Check; "
        "end E; "
        "public static class P "
        "  public static method Main(): int "
        "    return new E<long>(5L).Check(); "
        "  end Main; "
        "end P;"), 1);
}
