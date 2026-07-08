// Spec-derived tests for §27.7.1 Equality of function values (v0.180,
// issue #1544):
//
//   - A lambda (closure) value equals only itself — reference equality on
//     the closure instance. Two separately-constructed lambdas with
//     identical source text are NOT equal.
//   - A method reference equals another method reference IFF both denote
//     the SAME method and, for instance references, are bound to the SAME
//     receiver. A static (or extension-method) reference to M equals every
//     other reference to M. Repeated evaluations of `obj.Method` on the
//     same class-typed `obj` are GUARANTEED equal.
//   - A lambda never equals a method reference, and property references
//     follow the method-reference rule (the getter is the method).
//
// Before this fix, the general `=`/`<>` operator on two function-typed
// operands fell through to a raw LLVM pointer (`icmp eq`) comparison of the
// closure-CELL address. Every method/property reference formation
// (makeMethodThunk/makePropertyThunk) mints a FRESH closure cell (#1170's
// own comment: "a FRESH cell+env is built per reference"), so a static
// method reference never equalled another reference to the SAME method, and
// a repeated `obj.Method` on the same receiver never equalled itself either
// — silently violating the spec's "guaranteed equal" clause, even though the
// `-=` event-unsubscription desugaring (bz_event_remove_closure /
// bz_closure_same) already implemented the correct {fn, receiver} match.
// This also uncovered that property-reference thunks (makePropertyThunk)
// were never cached per property symbol the way method thunks are
// (methodThunks_, #1170) — fixed alongside (propertyThunks_) so property
// references compare equal for the same reasons.
//
// Each program returns int from Main (1/0 for the compared boolean) and is
// run via the in-process JIT.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// §27.7.1 first bullet: a lambda value equals itself — the SAME closure
// instance read through two different variables.
BEZEL_TEST(function_value_equality_1544,
          lambda_equals_itself_via_shared_binding_spec_27_7_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var f := () => 1; "
        "  var g := f; "
        "  return 1 if f = g else 0; "
        "end Main; end C;"), 1);
}

// §27.7.1 first bullet: "Two separately-constructed lambdas with identical
// source text are not equal, even if they capture the same variables."
BEZEL_TEST(function_value_equality_1544,
          distinct_lambdas_identical_source_not_equal_spec_27_7_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var f := () => 1; "
        "  var g := () => 1; "
        "  return 1 if f = g else 0; "
        "end Main; end C;"), 0);
}

// §27.7.1 second bullet: "A static ... reference to M equals every other
// reference to M."
BEZEL_TEST(function_value_equality_1544,
          static_method_reference_equals_another_reference_spec_27_7_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method M1(): int return 1; end M1; "
        "  public static method Main(): int "
        "    var f := C.M1; "
        "    var g := C.M1; "
        "    return 1 if f = g else 0; "
        "  end Main; "
        "end C;"), 1);
}

// §27.7.1 second bullet: "Repeated evaluations of obj.Method on the same
// class-typed obj are therefore guaranteed equal."
BEZEL_TEST(function_value_equality_1544,
          instance_reference_same_class_receiver_equal_spec_27_7_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Obj public method M1(): int return 1; end M1; end Obj; "
        "class C public static method Main(): int "
        "  using var o := new Obj(); "
        "  var f := o.M1; "
        "  var g := o.M1; "
        "  return 1 if f = g else 0; "
        "end Main; end C;"), 1);
}

// §27.7.1 second bullet (control): different class-typed receivers are NOT
// equal, even for the same method.
BEZEL_TEST(function_value_equality_1544,
          instance_reference_different_class_receivers_not_equal_spec_27_7_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Obj public method M1(): int return 1; end M1; end Obj; "
        "class C public static method Main(): int "
        "  using var o1 := new Obj(); "
        "  using var o2 := new Obj(); "
        "  var f := o1.M1; "
        "  var g := o2.M1; "
        "  return 1 if f = g else 0; "
        "end Main; end C;"), 0);
}

// §27.7.1 fourth bullet: "A lambda never equals a method reference."
BEZEL_TEST(function_value_equality_1544,
          lambda_never_equals_method_reference_spec_27_7_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method M1(): int return 1; end M1; "
        "  public static method Main(): int "
        "    var f: () -> int := C.M1; "
        "    var g: () -> int := () => 1; "
        "    return 1 if f = g else 0; "
        "  end Main; "
        "end C;"), 0);
}

// §27.7.1 fourth bullet: "property references follow the method-reference
// rule (the getter is the method)" — two property-getter references on the
// same class-typed receiver are equal.
BEZEL_TEST(function_value_equality_1544,
          property_reference_same_receiver_equal_spec_27_7_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public readonly property Count: int => 7; end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  var f: () -> int := b.Count; "
        "  var g: () -> int := b.Count; "
        "  return 1 if f = g else 0; "
        "end Main; end C;"), 1);
}

// §27.7.1: `<>` is the negation of `=` for function values — the SAME-method
// SAME-receiver case is not "not equal".
BEZEL_TEST(function_value_equality_1544,
          not_equal_operator_negates_equal_reference_spec_27_7_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method M1(): int return 1; end M1; "
        "  public static method Main(): int "
        "    var f := C.M1; "
        "    var g := C.M1; "
        "    return 1 if f <> g else 0; "
        "  end Main; "
        "end C;"), 0);
}

// §27.1.1's closing paragraph: "Assignability under this rule preserves
// IDENTITY: the value is the same function object viewed at a compatible
// function type ... so function-value equality (§27.7.1) is unaffected by
// the retyping." A () -> Dog value retyped (covariantly, §27.1.1) to
// () -> Animal still equals itself.
BEZEL_TEST(function_value_equality_1544,
          identity_preserved_across_covariant_retyping_spec_27_1_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal end Animal; "
        "class Dog: Animal end Dog; "
        "class C "
        "  public static method MakeDog(): Dog return new Dog(); end MakeDog; "
        "  public static method Main(): int "
        "    var f: () -> Dog := MakeDog; "
        "    var g: () -> Animal := f; "
        "    return 1 if f = g else 0; "
        "  end Main; "
        "end C;"), 1);
}
