// Regression tests for issue #5146 — spec-ahead-of-impl (v0.309, §12.7/§8.6):
// an operand whose DECLARED static type is a value type (or its nullable
// form) tested by `is`/`is not`/`as` against a REFERENCE target (a class, an
// interface it implements or not, `string`, or `Object`) is a compile error.
//
// spec §12.7 (v0.309), "Value-typed operands", verbatim:
//   "An operand whose static type is a value type or the nullable form of
//    one — a primitive, record, enum, choice, inline array, or the unit
//    type; for a carrier type-narrowed by this section, its declared type,
//    so a narrowed Object or interface carrier stays a boxed reference
//    operand — tested by is, is not, or as against a reference target (a
//    class, an interface — including one the value type implements —
//    string, or Object) is a compile error, as the §19.3 type pattern is:
//    the test could not discriminate at runtime, and answering it would
//    need an implicit boxing conversion, which a type-test operand never
//    performs. Box explicitly and test the box — `var o: Object := p; o is
//    IShape`"
//
// A type-parameter carrier `v: T`/`v: T?` is decided per instantiation
// instead (§3.6 rule 2, same section) and is NOT this rule's concern: the
// `as` half is rejected at the instantiation site
// (checkDeferredAsOperandInstantiations, ExprTypecheck.cpp); the `is`/`is
// not` static-decision-by-conformance half is a separate follow-up (not
// implemented by this fix — see the commit body).
//
// BEHAVIOR-FLIP SWEEP (mandatory per CLAUDE.md — a new reject rule):
//   * tests/codegen/type_test_unrelated_target_3410_test.cpp,
//     `value_unrelated_primitive_is_test_answers_false_spec_12_7`
//     (`var x: int := 5; if x is string then …`) FLIPS from accept-and-
//     answer-false to a compile error, and is updated alongside this fix.
//   * tests/codegen/type_test_is_string_generic_carrier_and_pipeline_4925_4909_test.cpp,
//     `generic_carrier_v_at_T_int_does_not_crash_spec_12_7` is named by the
//     issue as a flip (0 -> 1, once the `is`/`is not` generic conformance
//     decision lands); that half is UNIMPLEMENTED here (declined-with-
//     reasons, see commit body), so the row is UNCHANGED and still reads 0
//     — it stays a correct (if incomplete) answer, not a wrong one.
//   * tests/codegen/type_test_value_typed_operand_3120_test.cpp — every row
//     tests a value operand against a VALUE target (int/long/double/Pt/[N]int/
//     A vs B), never a reference target. Does NOT flip — reswept below.
//   * tests/codegen/type_param_is_as_narrowing_4812_test.cpp — every operand
//     is the bare type-parameter `v: T` (this rule's carve-out) or `Object`
//     (already a reference operand). Does NOT flip — reswept below.
//   * tests/codegen/concrete_is_as_decision_4858_test.cpp — every operand is
//     `string` or an interface/Object carrier (reference kinds already).
//     Does NOT flip — reswept below.
//
// ORACLE: reject rows assert compile failure AND the rule's own wording
// (unique in src/, `kRuleWording` below) so a coincidental unrelated
// rejection cannot pass the row. Accepted controls RUN and assert exact
// stdout.

#include "support/aot_support.hpp"  // runAot
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

constexpr const char* kRuleWording =
    "cannot be tested with";

bool rejectedByRule(const std::string& program) {
    auto r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(kRuleWording) != std::string::npos &&
            d.message.find("(\xC2\xA7""12.7)") != std::string::npos)
            return true;
    }
    return false;
}

// The generic-carrier `as`-operand instantiation-site rejection
// (checkDeferredAsOperandInstantiations) carries different wording — it
// names the instantiation site and §3.6 rule 2, not "cannot be tested with".
bool rejectedByGenericRule(const std::string& program) {
    auto r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("rejected at the instantiation site") !=
                std::string::npos &&
            d.message.find("(\xC2\xA7""3.6 rule 2)") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// =====================================================================
// REJECTED shapes.
// =====================================================================

// Primitive `int` operand vs `string` target, `is` — the issue's own
// motivating example (also pins tests/codegen/type_test_unrelated_target_3410).
BEZEL_TEST(value_typed_operand_reference_target_5146,
           primitive_is_string_rejected_spec_12_7) {
    BEZEL_EXPECT_TRUE(rejectedByRule(
        "module M; class C "
        "  public static method Main(): int "
        "    var x: int := 5; "
        "    if x is string then Console.WriteLine(\"yes\"); "
        "    else Console.WriteLine(\"no\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// Primitive vs `Object` — `is not`.
BEZEL_TEST(value_typed_operand_reference_target_5146,
           primitive_is_not_object_rejected_spec_12_7) {
    BEZEL_EXPECT_TRUE(rejectedByRule(
        "module M; class C "
        "  public static method Main(): int "
        "    var x: int := 5; "
        "    if x is not Object then Console.WriteLine(\"yes\"); "
        "    else Console.WriteLine(\"no\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// Primitive vs a user interface (one the primitive plainly does not
// implement) — `as`.
BEZEL_TEST(value_typed_operand_reference_target_5146,
           primitive_as_interface_rejected_spec_12_7) {
    BEZEL_EXPECT_TRUE(rejectedByRule(
        "module M; "
        "interface IShape5146 method Area(): int; end IShape5146; "
        "class C "
        "  public static method Main(): int "
        "    var x: int := 5; "
        "    var s := x as IShape5146; "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// Record operand vs a user CLASS target.
BEZEL_TEST(value_typed_operand_reference_target_5146,
           record_is_class_rejected_spec_12_7) {
    BEZEL_EXPECT_TRUE(rejectedByRule(
        "module M; "
        "record Pt5146(X: int, Y: int); "
        "class Widget5146 end Widget5146; "
        "class C "
        "  public static method Main(): int "
        "    var p: Pt5146 := new Pt5146(1, 2); "
        "    if p is Widget5146 then Console.WriteLine(\"yes\"); "
        "    else Console.WriteLine(\"no\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// Enum operand vs `Object`.
BEZEL_TEST(value_typed_operand_reference_target_5146,
           enum_is_object_rejected_spec_12_7) {
    BEZEL_EXPECT_TRUE(rejectedByRule(
        "module M; "
        "enum Col5146 Red, Green, Blue; end Col5146; "
        "class C "
        "  public static method Main(): int "
        "    var c: Col5146 := Col5146.Green; "
        "    if c is Object then Console.WriteLine(\"yes\"); "
        "    else Console.WriteLine(\"no\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// A record that DOES implement the interface tested — still rejected: §12.7
// draws no exception for a value type that implements the interface target
// ("including one the value type implements").
BEZEL_TEST(value_typed_operand_reference_target_5146,
           record_is_implemented_interface_still_rejected_spec_12_7) {
    BEZEL_EXPECT_TRUE(rejectedByRule(
        "module M; "
        "interface IShape5146b method Area(): int; end IShape5146b; "
        "record Pt5146b(X: int, Y: int): IShape5146b "
        "  public method Area(): int => X * Y; "
        "end Pt5146b; "
        "class C "
        "  public static method Main(): int "
        "    var p: Pt5146b := new Pt5146b(3, 4); "
        "    if p is IShape5146b then Console.WriteLine(\"yes\"); "
        "    else Console.WriteLine(\"no\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// Nullable primitive operand vs a reference target — the nullable form of a
// value type is covered too.
BEZEL_TEST(value_typed_operand_reference_target_5146,
           nullable_primitive_is_object_rejected_spec_12_7) {
    BEZEL_EXPECT_TRUE(rejectedByRule(
        "module M; class C "
        "  public static method Main(): int "
        "    var x: int? := 5; "
        "    if x is Object then Console.WriteLine(\"yes\"); "
        "    else Console.WriteLine(\"no\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// Inline-array operand vs `Object`.
BEZEL_TEST(value_typed_operand_reference_target_5146,
           inline_array_is_object_rejected_spec_12_7) {
    BEZEL_EXPECT_TRUE(rejectedByRule(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: [3]int := [1, 2, 3]; "
        "    if a is Object then Console.WriteLine(\"yes\"); "
        "    else Console.WriteLine(\"no\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// The unit type operand vs `Object`.
BEZEL_TEST(value_typed_operand_reference_target_5146,
           unit_is_object_rejected_spec_12_7) {
    BEZEL_EXPECT_TRUE(rejectedByRule(
        "module M; class C "
        "  public static method Main(): int "
        "    var u: () := (); "
        "    if u is Object then Console.WriteLine(\"yes\"); "
        "    else Console.WriteLine(\"no\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// =====================================================================
// ACCEPTED controls.
// =====================================================================

// Reference-typed operand (a class instance) vs an unrelated class — unaffected
// (control for tests/codegen/type_test_unrelated_target_3410).
BEZEL_TEST(value_typed_operand_reference_target_5146,
           reference_operand_is_unrelated_class_control_still_accepted) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class Dog5146 end Dog5146; "
        "class Widget5146c end Widget5146c; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Dog5146(); "
        "    if d is Widget5146c then Console.WriteLine(\"yes\"); "
        "    else Console.WriteLine(\"no\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("no"));
}

// A narrowed `Object` carrier keeps its DECLARED reference kind: `o += 1`
// (bare-binding compound assignment, unaffected by this rule) is not this
// test's concern, but the narrowING carrier itself (an `Object`-declared
// local, later narrowed to `int` inside an `is int` block) must still be
// admitted for a FOLLOW-ON `is`/`as` at its *declared* `Object` type outside
// the narrowed block — a value read through the declared location is the
// canonical accepted shape this rule is not supposed to reach.
BEZEL_TEST(value_typed_operand_reference_target_5146,
           object_carrier_declared_type_control_still_accepted) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    var o: Object := 5; "
        "    if o is int then Console.WriteLine(\"yes\"); "
        "    else Console.WriteLine(\"no\"); end; "
        "    if o is string then Console.WriteLine(\"str\"); "
        "    else Console.WriteLine(\"notstr\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("yes\nnotstr"));
}

// Value operand vs a VALUE target — unaffected (control for #3120's family;
// #3120 itself is reswept via its own file, unmodified).
BEZEL_TEST(value_typed_operand_reference_target_5146,
           value_operand_vs_value_target_control_still_accepted) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    var x: int := 7; "
        "    if x is int then Console.WriteLine(\"yes\"); "
        "    else Console.WriteLine(\"no\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("yes"));
}

// Generic type-parameter carrier `v: T` — NOT this rule's concern (per
// instantiation instead, §3.6 rule 2). `is` on a generic carrier against a
// reference target must still COMPILE (whatever it answers at runtime).
BEZEL_TEST(value_typed_operand_reference_target_5146,
           generic_carrier_is_reference_target_control_compiles) {
    auto r = runAot(
        "module M; "
        "public class Box5146 "
        "  public static method Peek<T>(v: T): int "
        "    if v is Object then return 1; end; "
        "    return 0; "
        "  end Peek; "
        "  public static method Main(): int "
        "    Console.WriteLine(Peek<int>(7)); "
        "    return 0; "
        "  end Main; "
        "end Box5146;");
    BEZEL_EXPECT_TRUE(r.ran);
}

// Generic type-parameter carrier `v: T` used as an `as` OPERAND against a
// concrete reference target, instantiated at a REFERENCE type argument —
// admitted (the ordinary runtime cast; the per-instantiation reject fires
// only for a VALUE substitution, pinned below).
BEZEL_TEST(value_typed_operand_reference_target_5146,
           generic_as_operand_reference_instantiation_accepted_spec_3_6_rule_2) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "public interface IShape5146d method Area(): int; end IShape5146d; "
        "public class Dog5146d: IShape5146d "
        "  public method Area(): int => 9; "
        "end Dog5146d; "
        "public class Box5146d "
        "  public static method Peek<T>(v: T): int "
        "    var s := v as IShape5146d; "
        "    if s <> null then return s.Area(); end; "
        "    return -1; "
        "  end Peek; "
        "  public static method Main(): int "
        "    using var d := new Dog5146d(); "
        "    Console.WriteLine(Peek<Dog5146d>(d)); "
        "    return 0; "
        "  end Main; "
        "end Box5146d;"), std::string("9"));
}

// The same generic `as`-operand-against-a-reference-target shape, but
// instantiated at a VALUE type argument — rejected AT THE INSTANTIATION SITE
// (§3.6 rule 2), not at the generic body's own declaration (`Peek`'s body
// type-checks fine under `T`'s weakest assumption; the call `Peek(7)` is
// where the substitution is known to be a value type with no boxing
// conversion to perform the cast with).
BEZEL_TEST(value_typed_operand_reference_target_5146,
           generic_as_operand_value_instantiation_rejected_spec_3_6_rule_2) {
    BEZEL_EXPECT_TRUE(rejectedByGenericRule(
        "module M; "
        "public interface IShape5146e method Area(): int; end IShape5146e; "
        "public class Box5146e "
        "  public static method Peek<T>(v: T): int "
        "    var s := v as IShape5146e; "
        "    if s <> null then return s.Area(); end; "
        "    return -1; "
        "  end Peek; "
        "  public static method Main(): int "
        "    Console.WriteLine(Peek<int>(7)); "
        "    return 0; "
        "  end Main; "
        "end Box5146e;"));
}
