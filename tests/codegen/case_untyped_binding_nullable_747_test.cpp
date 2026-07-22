// fix #747 R42 / §7 / §19.5 / §20: a `case` expression whose arms are a
// non-nullable arm (`null => 0` → int) and an untyped `var n` binding (which
// binds the WHOLE `int?` value, §19.5) has the §20 common type `int?`. Before
// the fix the common type was mis-resolved to the non-nullable `int`
// (commonTypeOf ignored the nullable flag), and codegen aborted the compiler
// with a PHI type-mismatch assertion. With the type correct, the case lowers to
// the `{i8 hasValue; int value}` nullable struct via the existing Path-A
// widening, so a value arm and the null arm both materialise the struct.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §19.5 / §20: untyped `var n` arm taken (x non-null) → result is the value 7.
BEZEL_TEST(codegen_case_untyped_binding_nullable_747, var_arm_value_path) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Main(): int "
        "    var x: int? := 7; "
        "    var r: int? := (case x of "
        "      null => 0, "
        "      var n => n); "
        "    return (if r = null then 99 else r!); "  // → 7
        "  end Main; "
        "end C;"), 7);
}

// §19.5 / §20: null arm taken (x null) → result is the non-null wrapped 0.
BEZEL_TEST(codegen_case_untyped_binding_nullable_747, null_arm_value_path) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Main(): int "
        "    var x: int? := null; "
        "    var r: int? := (case x of "
        "      null => 0, "
        "      var n => n); "
        "    return (if r = null then 99 else r! + 5); "  // 0 + 5 = 5
        "  end Main; "
        "end C;"), 5);
}

// §19.5: the bound name `n` carries the whole nullable value, so it can be
// re-tested for null inside the arm body and unwrapped — exercising that `n`
// really is `int?`, not a bare `int`.
BEZEL_TEST(codegen_case_untyped_binding_nullable_747, bound_name_is_nullable_value) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Main(): int "
        "    var x: int? := 11; "
        "    var r: int? := (case x of "
        "      var n => (if n = null then 100 else n!)); "
        "    return r!; "  // n is non-null 11 → 11
        "  end Main; "
        "end C;"), 11);
}

// CONTROL §19.5: a TYPED binding `var n: int` unwraps to a bare `int`, so the
// common type is `int` and the case may be returned from an `int` method.
BEZEL_TEST(codegen_case_untyped_binding_nullable_747, typed_binding_unwraps_to_int) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Main(): int "
        "    var x: int? := 42; "
        "    return (case x of "
        "      null => 0, "
        "      var n: int => n); "  // → 42
        "  end Main; "
        "end C;"), 42);
}
