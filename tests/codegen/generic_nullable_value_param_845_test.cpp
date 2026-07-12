// Codegen — issue #845: a generic method with a `T?` parameter (and a `T`
// return) using `??` to coalesce. Filed by the 2026-06-20 sweep as three
// failures (IR_FAIL / `??` returns 0 / runtime exception). All three were
// resolved by the nullable-value-parameter monomorphization work (#846 wrapping
// the bare argument into the tagged `{i8,T}` struct, #850 peeling `T?`→`T` for
// inference); these tests pin the exact reported repros against regression.
//
// §7 nullable values / §14.2 monomorphization. Each `??` coalesces a `T?`
// parameter against a `T` fallback inside a generic body and must return the
// substituted scalar (not the `{i8,T}` wrapper), matching the method's `T`
// return ABI.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// Repro A: `GetOrDefault<T>(v: T?, fallback: T): T where T: value` → `v ?? fallback`.
// Previously: IR verification failure (`ret {i8,i32}` vs `i32`).
BEZEL_TEST(generic_nullable_value_param_845, coalesce_t_nullable_param_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "static class U "
        "  public static method GetOrDefault<T>(v: T?, fallback: T): T where T: value "
        "    return v ?? fallback; "
        "  end GetOrDefault; "
        "end U; "
        "class C public static method Main(): int "
        "  var x: int? := 42; "
        "  return U.GetOrDefault<int>(x, 0); "  // 42
        "end Main; end C;"), 42);
}

// Repro B: a SECOND `T` (non-nullable) parameter binds the return ABI to i32, so
// the `??` over the `T?` arg must still read the value, not the default.
// Previously: returned 0.
BEZEL_TEST(generic_nullable_value_param_845, coalesce_with_second_value_param) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "static class U "
        "  public static method Pick<T>(flag: bool, nonNullable: T, nullable: T?): T where T: value "
        "    if flag then return nonNullable; end; "
        "    return nullable ?? default(T); "
        "  end Pick; "
        "end U; "
        "class C public static method Main(): int "
        "  var x: int? := 99; "
        "  return U.Pick<int>(false, 42, x); "  // 99
        "end Main; end C;"), 99);
}

// Repro C: a `long` instantiation (8-byte payload) — previously threw an
// unhandled runtime exception (PHI/type mismatch in the `??` lowering).
BEZEL_TEST(generic_nullable_value_param_845, coalesce_long_instantiation) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "static class U "
        "  public static method GetOrDefault<T>(v: T?, fallback: T): T "
        "    return v ?? fallback; "
        "  end GetOrDefault; "
        "end U; "
        "class C public static method Main(): int "
        "  var x: long? := 100L; "
        "  var r: long := U.GetOrDefault<long>(x, 0L); "
        "  return int(r); "  // 100
        "end Main; end C;"), 100);
}
