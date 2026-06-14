// Codegen — comparing a nullable TYPE-PARAMETER field (`T?`) to null inside a
// generic class method body. R33 discovery: `_value <> null` / `_value = null`
// where `_value: T?` lowered the operand to the nullable-value struct
// {i8 hasValue; <T> value} (the generic placeholder layout), but isNullableValueTy
// is false for the unresolved param, so the null comparison fell through to
// struct-equality with a null record type → an LLVM `checkGEPType` assertion
// (bezelc SIGABRT). Fix: detect the nullable-value struct by its LLVM shape
// (element 0 is the hasValue tag) and lower the null comparison to the hasValue
// check. Reproduced for both class and value instantiations of T.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// R33: `T? <> null` in a generic class method, T = a CLASS — previously crashed
// codegen (checkGEPType). The field is null, so HasValue() is false → 0.
BEZEL_TEST(codegen_generic_nullable_field_r33, compare_class_typeparam_nullable_to_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Maybe<T> "
        "  private _value: T?; "
        "  public constructor() _value := null; end constructor; "
        "  public method HasValue(): bool return _value <> null; end HasValue; "
        "  destructor end destructor; "
        "end Maybe; "
        "class Item public constructor() end constructor; destructor end destructor; end Item; "
        "class C public static method Main(): int "
        "  using var m := new Maybe<Item>(); "
        "  return (1 if m.HasValue() else 0); "
        "end Main; end C;"), 0);
}

// R33: same comparison with T = a VALUE type (int) — the nullable-value struct
// path; the field is null → HasValue() false → 0.
BEZEL_TEST(codegen_generic_nullable_field_r33, compare_value_typeparam_nullable_to_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Maybe<T> "
        "  private _value: T?; "
        "  public constructor() _value := null; end constructor; "
        "  public method IsNull(): bool return _value = null; end IsNull; "
        "end Maybe; "
        "class C public static method Main(): int "
        "  using var m := new Maybe<int>(); "
        "  return (1 if m.IsNull() else 0); "  // _value is null → IsNull true → 1
        "end Main; end C;"), 1);
}
