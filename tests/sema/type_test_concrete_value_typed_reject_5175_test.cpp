// Issue #5175: sema rejection of concrete value typed is/as against different value type
// Spec §12.7 (v0.310/#5157): a concrete value operand of `is`/`is not`/`as`
// against a different value type (not same type, not nullable form) is a compile error.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool compileErrored(const std::string& src) {
    return runAot(src).diagnostics.hasErrors();
}

// True iff some reported ERROR carries the #5175 diagnostic text (pins THE
// rule, not just "some error").
bool rejectedBy5175(const std::string& src) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("a concrete value-typed operand can only be tested") !=
            std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// §12.7 (v0.310/#5157): `int is long` — different concrete value types — compile error
BEZEL_TEST(type_test_concrete_value_typed_reject_5175, int_is_long_rejected) {
    BEZEL_EXPECT_EQ(compileErrored(
        "module M; class C "
        "  public static method Main(): int "
        "    var p: int := 7; "
        "    if p is long then return 99; end; "
        "    return 3; "
        "  end Main; "
        "end C;"), true);
}

// §12.7 (v0.310/#5157): `int is double` — compile error
BEZEL_TEST(type_test_concrete_value_typed_reject_5175, int_is_double_rejected) {
    BEZEL_EXPECT_EQ(compileErrored(
        "module M; class C "
        "  public static method Main(): int "
        "    var p: int := 7; "
        "    if p is double then return 99; end; "
        "    return 3; "
        "  end Main; "
        "end C;"), true);
}

// §12.7 (v0.310/#5157): `int is not long` — compile error (same shape as is)
BEZEL_TEST(type_test_concrete_value_typed_reject_5175, int_is_not_long_rejected) {
    BEZEL_EXPECT_EQ(compileErrored(
        "module M; class C "
        "  public static method Main(): int "
        "    var p: int := 7; "
        "    if p is not long then return 3; end; "
        "    return 99; "
        "  end Main; "
        "end C;"), true);
}

// §12.7 (v0.310/#5157): `int as long` — compile error
BEZEL_TEST(type_test_concrete_value_typed_reject_5175, int_as_long_rejected) {
    BEZEL_EXPECT_EQ(compileErrored(
        "module M; class C "
        "  public static method Main(): int "
        "    var p: int := 7; "
        "    var q: long? := p as long; "
        "    if q <> null then return 99; end; "
        "    return 3; "
        "  end Main; "
        "end C;"), true);
}

// §12.7 (v0.310/#5157): `int is Pt` where Pt is a record — compile error
BEZEL_TEST(type_test_concrete_value_typed_reject_5175, int_is_record_rejected) {
    BEZEL_EXPECT_EQ(compileErrored(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p: int := 7; "
        "    if p is Pt then return 99; end; "
        "    return 3; "
        "  end Main; "
        "end C;"), true);
}

// §12.7 (v0.310/#5157) control: `int is int` — same type — still admitted
BEZEL_TEST(type_test_concrete_value_typed_reject_5175, int_is_int_still_admitted) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var p: int := 7; "
        "    if p is int then return 1; end; "
        "    return 3; "
        "  end Main; "
        "end C;").exitCode, 1);
}

// §12.7 (v0.310/#5157) control: `int is int?` — own nullable form — still admitted
BEZEL_TEST(type_test_concrete_value_typed_reject_5175, int_is_int_nullable_still_admitted) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var p: int := 7; "
        "    if p is int? then return 1; end; "
        "    return 3; "
        "  end Main; "
        "end C;").exitCode, 1);
}

// §12.7 / §19.3 (#5170) control: generic `v is long` at T=int — still admitted
// (decides false per instantiation, not rejected)
BEZEL_TEST(type_test_concrete_value_typed_reject_5175, generic_carrier_is_long_at_int_admitted) {
    BEZEL_EXPECT_EQ(runAot(
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
        "  public static method Main(): int => new E<int>(5).Check(); "
        "end P;").exitCode, 3);
}

// §12.7 "Value-typed operands" (v0.313): a type-NARROWED Object carrier is
// read through its DECLARED type, so `o is long` after `o is int` is an
// ordinary boxed-reference test — accepted (runs: o holds an int, `is long`
// is false at runtime, so Main returns 0).
BEZEL_TEST(type_test_concrete_value_typed_reject_5175, narrowed_object_carrier_admitted) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var o: Object := 5; "
        "    if o is int then "
        "      if o is long then return 1; end; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;").exitCode, 0);
}

// §12.7 (v0.313): a type-narrowed INTERFACE carrier likewise stays a boxed
// reference operand.
BEZEL_TEST(type_test_concrete_value_typed_reject_5175, narrowed_interface_carrier_admitted) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; "
        "interface IShape5175 method Area(): int; end IShape5175; "
        "record Sq5175(S: int) implements IShape5175 "
        "  public method Area(): int => S * S; "
        "end Sq5175; "
        "class C "
        "  public static method Main(): int "
        "    var o: IShape5175 := new Sq5175(3); "
        "    if o is Sq5175 then "
        "      if o is long then return 1; end; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;").exitCode, 0);
}

// §12.7 (v0.313): the nullable form of a value type is a value-typed operand;
// only its own type / own nullable form is admitted as a target.
BEZEL_TEST(type_test_concrete_value_typed_reject_5175, nullable_operand_is_other_value_type_rejected) {
    BEZEL_EXPECT_TRUE(rejectedBy5175(
        "module M; class C "
        "  public static method Main(): int "
        "    var p: int? := 5; "
        "    if p is long then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

BEZEL_TEST(type_test_concrete_value_typed_reject_5175, nullable_operand_as_other_value_type_rejected) {
    BEZEL_EXPECT_TRUE(rejectedBy5175(
        "module M; class C "
        "  public static method Main(): int "
        "    var p: int? := 5; "
        "    var q := p as long; "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// control: nullable operand against its own type / own nullable form admitted
BEZEL_TEST(type_test_concrete_value_typed_reject_5175, nullable_operand_own_type_admitted) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var p: int? := 5; "
        "    if p is int then return 4; end; "
        "    return 0; "
        "  end Main; "
        "end C;").exitCode, 4);
}

// §12.7 (v0.313): a concrete `p: int` inside a generic METHOD is still a
// concrete value operand — `p is long` is rejected (only a type parameter on
// either side defers the test to the instantiation).
BEZEL_TEST(type_test_concrete_value_typed_reject_5175, concrete_operand_in_generic_method_rejected) {
    BEZEL_EXPECT_TRUE(rejectedBy5175(
        "module M; class C "
        "  public static method F<T>(v: T): int "
        "    var p: int := 5; "
        "    if p is long then return 1; end; "
        "    return 0; "
        "  end F; "
        "  public static method Main(): int "
        "    return C.F<int>(3); "
        "  end Main; "
        "end C;"));
}

// §12.7 (v0.313) control: a type-parameter carrier `v: T` against a concrete
// value target in a generic method is decided per instantiation — admitted.
BEZEL_TEST(type_test_concrete_value_typed_reject_5175, type_param_carrier_in_generic_method_admitted) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method F<T>(v: T): int "
        "    if v is long then return 1; end; "
        "    return 0; "
        "  end F; "
        "  public static method Main(): int "
        "    return C.F<int>(3); "
        "  end Main; "
        "end C;").exitCode, 0);
}
