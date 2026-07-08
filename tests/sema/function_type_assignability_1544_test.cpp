// Spec-derived tests for §27.1.1 Function-type assignability (v0.180,
// issue #1544): a function-typed VALUE (a value already held in a variable —
// as opposed to a fresh method/property reference FORMED at the assignment
// site, §27.6) may be used where a different function type is expected under
// one narrow variance rule:
//   - class/interface PARAMETER slots are CONTRAVARIANT.
//   - the class/interface RETURN slot is COVARIANT.
//   - value-typed/record-typed/primitive slots are INVARIANT (no widening,
//     boxing, or other representation-changing conversion).
//   - modifier slots (owned/ref/out/out owned/scoped) must match EXACTLY.
//
// Before this fix, checkAssignable's Function-Function branch (ExprTypecheck.
// cpp) checked only `scoped`-modifier agreement and otherwise passed any
// function-to-function assignment through silently — accepting mismatched
// arity, mismatched non-scoped modifiers, unrelated class types, and
// representation-changing primitive conversions that a `(long) -> int`-typed
// call through an `(int) -> int`-backed closure then executed with a wrong-
// width argument (undefined behavior at the ABI boundary — no adapter exists
// for an already-built function value, §27.1.1 third bullet).
//
// The full front-end (through the ownership pass) runs on each program; the
// positives assert !hasErrors(), the negatives assert hasErrors().

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

bool diagContains(const bezel::test::OwnershipResult& r,
                  const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// §27.1.1 first bullet, spec example: "a (Animal) -> () value is usable
// where (Dog) -> () is expected" — the target's (narrower) param type
// reference-converts to the value's (wider) param type.
BEZEL_TEST(function_type_assignability_1544,
          contravariant_param_widening_accepted_spec_27_1_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Animal end Animal; "
        "class Dog: Animal end Dog; "
        "class C public static method Main(): int "
        "  var f: (Animal) -> () := (a: Animal) => do end; "
        "  var g: (Dog) -> () := f; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.1.1 first bullet, reverse direction: a (Dog) -> () value is NOT usable
// where (Animal) -> () is expected — calling the result with a non-Dog
// Animal would call the Dog-only handler on the wrong receiver shape.
BEZEL_TEST(function_type_assignability_1544,
          contravariant_param_narrowing_rejected_spec_27_1_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Animal end Animal; "
        "class Dog: Animal end Dog; "
        "class C public static method Main(): int "
        "  var f: (Dog) -> () := (d: Dog) => do end; "
        "  var g: (Animal) -> () := f; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §27.1.1 second bullet, spec example: "a () -> Dog value is usable as
// () -> Animal" — the value's (narrower) return type reference-converts to
// the target's (wider) return type.
BEZEL_TEST(function_type_assignability_1544,
          covariant_return_narrowing_accepted_spec_27_1_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Animal end Animal; "
        "class Dog: Animal end Dog; "
        "class C "
        "  public static method MakeDog(): Dog return new Dog(); end MakeDog; "
        "  public static method Main(): int "
        "    var f: () -> Dog := MakeDog; "
        "    var g: () -> Animal := f; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.1.1 second bullet, reverse direction: a () -> Animal value is NOT
// usable as () -> Dog — the caller would receive a non-Dog Animal typed
// as Dog.
BEZEL_TEST(function_type_assignability_1544,
          covariant_return_widening_rejected_spec_27_1_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Animal end Animal; "
        "class Dog: Animal end Dog; "
        "class C "
        "  public static method MakeAnimal(): Animal return new Animal(); end MakeAnimal; "
        "  public static method Main(): int "
        "    var f: () -> Animal := MakeAnimal; "
        "    var g: () -> Dog := f; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §27.1.1 third bullet: "value-typed ... primitive slots are INVARIANT. No
// numeric widening ... applies to a slot of an already-constructed function
// value: there is no adapter object to perform it." An (int) -> int VALUE is
// not usable where (long) -> int is expected, even though int widens to long
// as an ordinary value conversion (§5.1).
BEZEL_TEST(function_type_assignability_1544,
          primitive_param_invariant_rejected_spec_27_1_1) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Twice(x: int): int return x * 2; end Twice; "
        "  public static method Main(): int "
        "    var f: (int) -> int := Twice; "
        "    var g: (long) -> int := f; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §27.1.1 third bullet, RETURN slot: a () -> int value is not usable as
// () -> long — same invariance, applied to the return slot.
BEZEL_TEST(function_type_assignability_1544,
          primitive_return_invariant_rejected_spec_27_1_1) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method GetInt(): int return 5; end GetInt; "
        "  public static method Main(): int "
        "    var f: () -> int := GetInt; "
        "    var g: () -> long := f; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §27.1.1 third bullet: a representation-changing conversion between
// unrelated primitive kinds (int vs double — different register class, not
// just a different width) is rejected the same way.
BEZEL_TEST(function_type_assignability_1544,
          representation_changing_param_rejected_spec_27_1_1) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Twice(x: int): int return x * 2; end Twice; "
        "  public static method Main(): int "
        "    var f: (int) -> int := Twice; "
        "    var g: (double) -> int := f; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §27.1.1 (arity is a prerequisite for every bullet above): a mismatched
// arity is never assignable.
BEZEL_TEST(function_type_assignability_1544,
          arity_mismatch_rejected_spec_27_1_1) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Twice(x: int): int return x * 2; end Twice; "
        "  public static method Main(): int "
        "    var f: (int) -> int := Twice; "
        "    var g: (int, int) -> int := f; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §27.1.1 first bullet (no relation at all): a param type that is neither a
// sub- nor a supertype of the target's is rejected — Animal and Widget share
// no hierarchy edge.
BEZEL_TEST(function_type_assignability_1544,
          unrelated_class_types_rejected_spec_27_1_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Animal end Animal; "
        "class Widget end Widget; "
        "class C "
        "  public static method Feed(a: Animal) return; end Feed; "
        "  public static method Main(): int "
        "    var f: (Animal) -> () := Feed; "
        "    var g: (Widget) -> () := f; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §27.1.1 fourth bullet: modifier slots must match EXACTLY — `ref` is not
// interchangeable with a plain (unmodified) slot.
BEZEL_TEST(function_type_assignability_1544,
          ref_modifier_mismatch_rejected_spec_27_1_1) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Bump(ref x: int) x := x + 1; end Bump; "
        "  public static method Main(): int "
        "    var f: (ref int) -> () := Bump; "
        "    var g: (int) -> () := f; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "identity"));
}

// §27.1.1 fourth bullet: `owned` likewise must match exactly.
BEZEL_TEST(function_type_assignability_1544,
          owned_modifier_mismatch_rejected_spec_27_1_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box end Box; "
        "class C "
        "  public static method Take(owned b: Box) end Take; "
        "  public static method Main(): int "
        "    var f: (owned Box) -> () := Take; "
        "    var g: (Box) -> () := f; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "identity"));
}

// §27.1.1 third bullet: distinct RECORD types are invariant — R2 is not R1
// even though both are single-int positional records.
BEZEL_TEST(function_type_assignability_1544,
          record_param_invariant_rejected_spec_27_1_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record R1(X: int); "
        "record R2(X: int); "
        "class C "
        "  public static method TakeR1(r: R1): int return r.X; end TakeR1; "
        "  public static method Main(): int "
        "    var f: (R1) -> int := TakeR1; "
        "    var g: (R2) -> int := f; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Control: an IDENTICAL record-typed signature is accepted (invariance
// rejects a DIFFERENT record, not the reassignment itself).
BEZEL_TEST(function_type_assignability_1544,
          record_param_identical_type_accepted_spec_27_1_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record R1(X: int); "
        "class C "
        "  public static method TakeR1(r: R1): int return r.X; end TakeR1; "
        "  public static method Main(): int "
        "    var f: (R1) -> int := TakeR1; "
        "    var g: (R1) -> int := f; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Control: an identical (int) -> int signature reassignment is accepted —
// the invariance rule rejects a DIFFERENT type, not same-type reuse.
BEZEL_TEST(function_type_assignability_1544,
          identical_signature_reassignment_accepted_spec_27_1_1) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Twice(x: int): int return x * 2; end Twice; "
        "  public static method Main(): int "
        "    var f: (int) -> int := Twice; "
        "    var g: (int) -> int := f; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.1.1 fifth paragraph / §27.6: method-reference FORMATION at the
// assignment site is a DIFFERENT (broader) rule than value-assignability —
// "forming a reference from a named method against a target type may bridge
// any §5.1 implicit conversion, because the compiler synthesizes the adapter
// at the formation site." A direct `var g: (long) -> int := Twice;` (Twice's
// own natural type is (int) -> int) is therefore NOT rejected by the §27.1.1
// invariance rule that rejects the same shape mismatch once the value is
// already held in a variable (see primitive_param_invariant_rejected above).
BEZEL_TEST(function_type_assignability_1544,
          formation_site_bridges_widening_spec_27_6) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Twice(x: int): int return x * 2; end Twice; "
        "  public static method Main(): int "
        "    var g: (long) -> int := Twice; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.1.1 second bullet + §7: nullable widening composes with covariant
// return — a non-null () -> Dog value is usable as a nullable
// (() -> Animal)? target.
BEZEL_TEST(function_type_assignability_1544,
          covariant_return_nullable_target_accepted_spec_27_1_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Animal end Animal; "
        "class Dog: Animal end Dog; "
        "class C "
        "  public static method MakeDog(): Dog return new Dog(); end MakeDog; "
        "  public static method Main(): int "
        "    var f: () -> Dog := MakeDog; "
        "    var g: (() -> Animal)? := f; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7 (general nullable rule, unaffected by the Function-Function block
// existing): a nullable function-typed value is not usable where a
// non-nullable function type is expected.
BEZEL_TEST(function_type_assignability_1544,
          nullable_function_value_rejected_for_non_nullable_target_spec_7) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Twice(x: int): int return x * 2; end Twice; "
        "  public static method Main(): int "
        "    var f: ((int) -> int)? := Twice; "
        "    var g: (int) -> int := f; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §27.2 / §28.14 (v0.175) control, re-verified alongside the v0.180
// generalization above: matching `scoped` positions are still accepted, and
// the mismatch diagnostic still names `scoped` specifically (not just
// "modifier N").
BEZEL_TEST(function_type_assignability_1544,
          scoped_modifier_mismatch_still_named_explicitly_spec_27_2) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Take(scoped f: () -> ()) f(); end Take; "
        "  public static method Main(): int "
        "    var h: (() -> ()) -> () := C.Take; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "`scoped`"));
}
