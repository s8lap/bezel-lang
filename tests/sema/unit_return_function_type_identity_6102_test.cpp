// fix #6102 (§5.1 identical-type conversion, §27.1 function types, v0.338):
// a generic argument whose function type has a unit return `()` was never
// identical to itself. `typeArgSameBase` had no arm for `TypeRef::Kind::Unit`,
// so two `()` return refs compared unequal, and `Array<(int) -> ()>` passed to
// a parameter of the identical type was rejected as "not implicitly
// convertible to the target type (§5.1)". The `-> int` control compiled.
//
// Rows:
//   A1  REPRO: local `Array<(int) -> ()>` passed to a parameter of the same
//       type. Accepted and runs.
//   A2  CONTROL: local `Array<(int) -> int>` to the same shape. Accepted and
//       runs (already passed before the fix).
//   A3  field `Array<(int) -> ()>` passed to a parameter of the same type.
//   A4  `Array<() -> ()>` (zero-parameter unit-returning function) to the same.
//   A5  user generic `Box<(int) -> ()>` passed to a parameter of the same type.
//   R1  `Array<(int) -> ()>` to `Array<(string) -> ()>`: different parameter
//       type, must stay rejected.
//   R2  `Array<(int) -> ()>` to `Array<(int) -> int>`: different return type
//       (unit vs int), must stay rejected.
//   R3  `Array<(int) -> ()>` to `Array<() -> ()>`: different parameter count,
//       must stay rejected.
//   A6  `Array<((int) -> ())?>` to the identical nullable parameter: accepted.
//   R4  `Array<((int) -> ())?>` to `Array<(int) -> ()>`: nullable element vs
//       non-nullable, §12.9 invariance, must reject.
//   R5  mirror of R4: `Array<(int) -> ()>` to `Array<((int) -> ())?>`.
//   R6  nullable unit return: `Array<(int) -> ()?>` to `Array<(int) -> ()>`.
//   R7  `Array<((int) -> int)?>` to `Array<(int) -> int>`: a PRE-EXISTING false
//       accept on base (non-unit return); the nullability check in
//       typeArgSameBase rejects it. Fails on base by design.
//   R8  mirror of R7.
//   A7  RUN: the accepted `Array<(int) -> ()>` is passed to the identical
//       parameter and the method calls an element (stdout asserted).
// Nullability is part of the element type: the Unit arm and the Function arm of
// typeArgSameBase compare `nullable` (§5.1/§12.9 invariance). Without it the
// unit fix would accept R4/R5/R6 (different types). The primitive
// nullable-return gap (`Array<(int) -> int?>` vs `Array<(int) -> int>`) is NOT
// covered here; it is tracked as #6109.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

// ── A1 — REPRO: Array<(int) -> ()> passed to an identical parameter.
BEZEL_TEST(sema_unit_return_function_type_identity_6102,
           array_proc_local_to_identical_param_spec_5_1_v0_338) {
    auto r = runAot(
        "module M6102A1; "
        "class K "
        "  public constructor() end constructor; "
        "  public method P(a: Array<(int) -> ()>): int "
        "    return a.Count; "
        "  end P; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var fs: Array<(int) -> ()> := []; "
        "    Console.WriteLine(k.P(fs)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// ── A2 — CONTROL: the same shape with an `int` return compiles and runs.
BEZEL_TEST(sema_unit_return_function_type_identity_6102,
           array_proc_int_return_control_spec_5_1_v0_338) {
    auto r = runAot(
        "module M6102A2; "
        "class K "
        "  public constructor() end constructor; "
        "  public method P(a: Array<(int) -> int>): int "
        "    return a.Count; "
        "  end P; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var fs: Array<(int) -> int> := []; "
        "    Console.WriteLine(k.P(fs)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// ── A3 — a FIELD of type Array<(int) -> ()> passed to an identical parameter.
BEZEL_TEST(sema_unit_return_function_type_identity_6102,
           array_proc_field_to_identical_param_spec_5_1_v0_338) {
    auto r = runAot(
        "module M6102A3; "
        "class K "
        "  public constructor() end constructor; "
        "  public method P(a: Array<(int) -> ()>): int "
        "    return a.Count; "
        "  end P; "
        "end K; "
        "class H "
        "  public fs: Array<(int) -> ()> := []; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var h := new H(); "
        "    Console.WriteLine(k.P(h.fs)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// ── A4 — Array<() -> ()> (zero-parameter, unit-returning) to an identical
// parameter.
BEZEL_TEST(sema_unit_return_function_type_identity_6102,
           array_zero_arg_proc_local_to_identical_param_spec_5_1_v0_338) {
    auto r = runAot(
        "module M6102A4; "
        "class K "
        "  public constructor() end constructor; "
        "  public method P(a: Array<() -> ()>): int "
        "    return a.Count; "
        "  end P; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var fs: Array<() -> ()> := []; "
        "    Console.WriteLine(k.P(fs)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// ── A5 — a user generic Box<(int) -> ()> passed to an identical parameter.
BEZEL_TEST(sema_unit_return_function_type_identity_6102,
           user_generic_box_proc_to_identical_param_spec_5_1_v0_338) {
    auto r = runAot(
        "module M6102A5; "
        "class Box<T> "
        "  public x: T; "
        "  public constructor(v: T) x := v; end constructor; "
        "end Box; "
        "class K "
        "  public constructor() end constructor; "
        "  public method P(a: Box<(int) -> ()>): int "
        "    return 0; "
        "  end P; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var b := new Box<(int) -> ()>(lambda(x: int) => do end); "
        "    Console.WriteLine(k.P(b)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// ── R1 — Array<(int) -> ()> to Array<(string) -> ()> stays rejected: the
// parameter types differ, so this is a genuinely different function type.
BEZEL_TEST(sema_unit_return_function_type_identity_6102,
           array_proc_int_to_string_param_still_rejected_spec_5_1_v0_338) {
    auto r = runAot(
        "module M6102R1; "
        "class K "
        "  public constructor() end constructor; "
        "  public method P(a: Array<(string) -> ()>): int "
        "    return 0; "
        "  end P; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var fs: Array<(int) -> ()> := []; "
        "    Console.WriteLine(k.P(fs)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `a` to call to `P` has a type that is not implicitly "
           "convertible to the target type (§5.1)"));
}

// ── R2 — Array<(int) -> ()> to Array<(int) -> int> stays rejected: the return
// types differ (unit vs int).
BEZEL_TEST(sema_unit_return_function_type_identity_6102,
           array_proc_to_int_return_param_still_rejected_spec_5_1_v0_338) {
    auto r = runAot(
        "module M6102R2; "
        "class K "
        "  public constructor() end constructor; "
        "  public method P(a: Array<(int) -> int>): int "
        "    return 0; "
        "  end P; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var fs: Array<(int) -> ()> := []; "
        "    Console.WriteLine(k.P(fs)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `a` to call to `P` has a type that is not implicitly "
           "convertible to the target type (§5.1)"));
}

// ── R3 — Array<(int) -> ()> to Array<() -> ()> stays rejected: the parameter
// counts differ.
BEZEL_TEST(sema_unit_return_function_type_identity_6102,
           array_proc_to_zero_arg_param_still_rejected_spec_5_1_v0_338) {
    auto r = runAot(
        "module M6102R3; "
        "class K "
        "  public constructor() end constructor; "
        "  public method P(a: Array<() -> ()>): int "
        "    return 0; "
        "  end P; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var fs: Array<(int) -> ()> := []; "
        "    Console.WriteLine(k.P(fs)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `a` to call to `P` has a type that is not implicitly "
           "convertible to the target type (§5.1)"));
}

// ── A6 — Array<((int) -> ())?> to an identical nullable parameter: the element
// types are identical (both nullable), so this stays accepted.
BEZEL_TEST(sema_unit_return_function_type_identity_6102,
           array_nullable_proc_to_identical_nullable_param_spec_5_1_v0_338) {
    auto r = runAot(
        "module M6102A6; "
        "class K "
        "  public constructor() end constructor; "
        "  public method P(a: Array<((int) -> ())?>): int "
        "    return a.Count; "
        "  end P; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var fs: Array<((int) -> ())?> := []; "
        "    Console.WriteLine(k.P(fs)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// ── R4 — Array<((int) -> ())?> (nullable element) to Array<(int) -> ()>
// (non-nullable element) stays rejected: §12.9 invariance, nullability is part
// of the element type.
BEZEL_TEST(sema_unit_return_function_type_identity_6102,
           array_nullable_proc_to_nonnull_param_rejected_spec_5_1_v0_338) {
    auto r = runAot(
        "module M6102R4; "
        "class K "
        "  public constructor() end constructor; "
        "  public method P(a: Array<(int) -> ()>): int "
        "    return 0; "
        "  end P; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var fs: Array<((int) -> ())?> := []; "
        "    Console.WriteLine(k.P(fs)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `a` to call to `P` has a type that is not implicitly "
           "convertible to the target type (§5.1)"));
}

// ── R5 — Array<(int) -> ()> (non-nullable element) to Array<((int) -> ())?>
// stays rejected: the mirror of R4.
BEZEL_TEST(sema_unit_return_function_type_identity_6102,
           array_nonnull_proc_to_nullable_param_rejected_spec_5_1_v0_338) {
    auto r = runAot(
        "module M6102R5; "
        "class K "
        "  public constructor() end constructor; "
        "  public method P(a: Array<((int) -> ())?>): int "
        "    return 0; "
        "  end P; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var fs: Array<(int) -> ()> := []; "
        "    Console.WriteLine(k.P(fs)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `a` to call to `P` has a type that is not implicitly "
           "convertible to the target type (§5.1)"));
}

// ── R6 — nullable UNIT RETURN: Array<(int) -> ()?> to Array<(int) -> ()> stays
// rejected (the return type `()?` differs from `()`).
BEZEL_TEST(sema_unit_return_function_type_identity_6102,
           array_nullable_unit_return_to_nonnull_rejected_spec_5_1_v0_338) {
    auto r = runAot(
        "module M6102R6; "
        "class K "
        "  public constructor() end constructor; "
        "  public method P(a: Array<(int) -> ()>): int "
        "    return 0; "
        "  end P; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var fs: Array<(int) -> ()?> := []; "
        "    Console.WriteLine(k.P(fs)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `a` to call to `P` has a type that is not implicitly "
           "convertible to the target type (§5.1)"));
}

// ── R7 — Array<((int) -> int)?> to Array<(int) -> int>: pre-existing false
// accept on base (non-unit return, nullable element). Now rejected: §12.9
// invariance includes the element's nullability.
BEZEL_TEST(sema_unit_return_function_type_identity_6102,
           array_nullable_int_proc_to_nonnull_param_rejected_spec_5_1_v0_338) {
    auto r = runAot(
        "module M6102R7; "
        "class K "
        "  public constructor() end constructor; "
        "  public method P(a: Array<(int) -> int>): int "
        "    return 0; "
        "  end P; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var fs: Array<((int) -> int)?> := []; "
        "    Console.WriteLine(k.P(fs)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `a` to call to `P` has a type that is not implicitly "
           "convertible to the target type (§5.1)"));
}

// ── R8 — Array<(int) -> int> to Array<((int) -> int)?>: the mirror of R7 on
// base (also a pre-existing false accept). Now rejected.
BEZEL_TEST(sema_unit_return_function_type_identity_6102,
           array_nonnull_int_proc_to_nullable_param_rejected_spec_5_1_v0_338) {
    auto r = runAot(
        "module M6102R8; "
        "class K "
        "  public constructor() end constructor; "
        "  public method P(a: Array<((int) -> int)?>): int "
        "    return 0; "
        "  end P; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var fs: Array<(int) -> int> := []; "
        "    Console.WriteLine(k.P(fs)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `a` to call to `P` has a type that is not implicitly "
           "convertible to the target type (§5.1)"));
}

// ── A7 — RUN: the accepted Array<(int) -> ()> is passed to a parameter of the
// identical type and the method calls an element. Pins that the identity
// accept is not just a typing accept: the value reaches codegen and runs.
BEZEL_TEST(sema_unit_return_function_type_identity_6102,
           array_proc_passed_and_element_called_runs_spec_5_1_v0_338) {
    auto r = runAot(
        "module M6102A7; "
        "class K "
        "  public constructor() end constructor; "
        "  public method P(a: Array<(int) -> ()>): int "
        "    var f := a[0]; "
        "    f(7); "
        "    return a.Count; "
        "  end P; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var fs: Array<(int) -> ()> := []; "
        "    fs.Add(lambda(x: int) => do Console.WriteLine(x.ToString()); end); "
        "    Console.WriteLine(k.P(fs).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n1\n"));
}
