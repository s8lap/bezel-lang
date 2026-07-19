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
// FOLLOW-UP (#1544 residual gap, closed): the {fn, receiver} match alone was
// still wrong in two cases because the runtime could not tell a lambda from a
// method reference, nor a class-receiver reference from a record-receiver
// reference (all are {fn, env, rc, envdtor} cells):
//   (A) two DISTINCT closures from the SAME lambda site (e.g. a method that
//       returns a lambda, called twice) compared EQUAL — same thunk fn + null
//       envs — violating "a lambda equals only itself";
//   (B) two separate evaluations of a record-receiver method/property
//       reference compared EQUAL — same fn + matching env bytes — violating
//       the third bullet ("two separate evaluations compare unequal").
// The fix tags each closure cell with a function-value KIND (FnValKind, field
// index 4 / off 32) and dispatches bz_closure_same on it: a lambda /
// value-receiver reference uses reference identity (equals only itself); a
// class-receiver reference uses fn + receiver; a static reference uses fn.
// The tests below (same-site lambdas, record-receiver refs) PIN both gaps.
//
// Each program returns int from Main (1/0 for the compared boolean) and is
// run via the in-process JIT.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
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
// () -> Animal still equals itself. v0.201 (#2048): the fresh-producing
// factory is spelled `owned Dog` per the declaration-site static-return rule;
// the function-typed slots follow under §27.1.1's exact-modifier matching.
BEZEL_TEST(function_value_equality_1544,
          identity_preserved_across_covariant_retyping_spec_27_1_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal end Animal; "
        "class Dog: Animal end Dog; "
        "class C "
        "  public static method MakeDog(): owned Dog return new Dog(); end MakeDog; "
        "  public static method Main(): int "
        "    var f: () -> owned Dog := MakeDog; "
        "    var g: () -> owned Animal := f; "
        "    return 1 if f = g else 0; "
        "  end Main; "
        "end C;"), 1);
}

// ── #1544 residual gap (closed): the {fn, receiver} match alone could not
// distinguish a lambda's identity from a method reference's {fn, receiver}
// pair. These PIN the two cases it got wrong before the FnValKind tag. ──

// §27.7.1 first bullet: "Two separately-constructed lambdas with identical
// source text are not equal, even if they capture the same variables." The
// distinct-lambda test earlier in this file used TWO different source lambda
// literals, which get different thunks and so passed even without the fix.
// This one constructs two closures from the SAME lambda site (a method that
// returns a lambda, called twice): they share the thunk fn, so before the
// FnValKind tag they compared EQUAL (same fn + null envs) — violating the
// "lambda equals only itself" rule. Now the lambda cell is tagged
// FVK_REF_IDENTITY and two distinct cells are unequal.
BEZEL_TEST(function_value_equality_1544,
          same_site_lambdas_distinct_cells_not_equal_spec_27_7_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method MakeL(): () -> int return () => 42; end MakeL; "
        "  public static method Main(): int "
        "    var f := MakeL(); "
        "    var g := MakeL(); "
        "    return 1 if f = g else 0; "
        "  end Main; "
        "end C;"), 0);
}

// §27.7.1 first bullet (complement): a closure read through two bindings IS
// the same function object (function values are reference-typed — assignment
// aliases the cell), so it equals itself. The store-once pattern of §24.4.
BEZEL_TEST(function_value_equality_1544,
          same_site_lambda_stored_once_equals_itself_spec_27_7_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method MakeL(): () -> int return () => 42; end MakeL; "
        "  public static method Main(): int "
        "    var f := MakeL(); "
        "    var g := f; "
        "    return 1 if f = g else 0; "
        "  end Main; "
        "end C;"), 1);
}

// §27.7.1 third bullet: "A method reference whose receiver is value-shaped (a
// record) ... has no receiver identity: two separate evaluations compare
// UNEQUAL (each holds a distinct copy)." Before the FnValKind tag, two
// evaluations of `r.GetV` shared the thunk fn AND the env's first bytes (the
// record copy), so they wrongly compared EQUAL. Now a record-receiver
// reference is tagged FVK_REF_IDENTITY and two distinct cells are unequal.
BEZEL_TEST(function_value_equality_1544,
          record_receiver_method_ref_separate_evals_not_equal_spec_27_7_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record R(Value: int) "
        "  public method GetV(): int return Value; end GetV; "
        "end R; "
        "class C public static method Main(): int "
        "  var r := new R(5); "
        "  var f := r.GetV; "
        "  var g := r.GetV; "
        "  return 1 if f = g else 0; "
        "end Main; end C;"), 0);
}

// §27.7.1 third bullet (complement): the SAME record-receiver reference value
// (stored once) equals itself — "only the same function object equals itself".
// This is why the store-once pattern of §24.4 is *required* for record
// receivers and works for `-=`: the stored value compared to itself is the
// identical cell.
BEZEL_TEST(function_value_equality_1544,
          record_receiver_method_ref_stored_once_equals_itself_spec_27_7_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record R(Value: int) "
        "  public method GetV(): int return Value; end GetV; "
        "end R; "
        "class C public static method Main(): int "
        "  var r := new R(5); "
        "  var f := r.GetV; "
        "  var g := f; "
        "  return 1 if f = g else 0; "
        "end Main; end C;"), 1);
}

// §27.7.1 fourth bullet: "A lambda never equals a method reference." With the
// FnValKind tag this is enforced structurally: a lambda is FVK_REF_IDENTITY
// and a class-receiver method reference is FVK_FN_PLUS_RECV — distinct kinds
// are never equal, so a same-site lambda and a same-receiver method reference
// (whose thunks differ anyway) compare unequal.
BEZEL_TEST(function_value_equality_1544,
          lambda_never_equals_class_method_reference_spec_27_7_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Obj public method M1(): int return 1; end M1; end Obj; "
        "class C public static method Main(): int "
        "  using var o := new Obj(); "
        "  var f: () -> int := () => 1; "
        "  var g: () -> int := o.M1; "
        "  return 1 if f = g else 0; "
        "end Main; end C;"), 0);
}
