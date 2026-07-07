// Spec-derived tests for §12.9 variance position enforcement.
//
// §12.9: `out` requires T to appear only in output positions (return types,
// read-only property types, `out` parameters). `in` requires T to appear
// only in input positions (method parameter types, not `out` parameters).
// The compiler enforces position constraints.
//
// Tests fall into four groups:
//   A) `out T` violations — T in input position → rejected.
//   B) `in T` violations — T in output position → rejected.
//   C) Valid uses of `out T` in output positions → accepted.
//   D) Valid uses of `in T` in input positions → accepted.
//   E) Regression: invariant interfaces and non-interfaces compile normally.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some diagnostic is a §12.9 variance position violation.
bool hasVarianceError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("12.9") != std::string::npos ||
            d.message.find("variance") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// True iff some diagnostic message contains `needle` (issue #1559 tests use
// this to check for the specific "outside interface" / §11.8 wording rather
// than the generic §12.9-position substring `hasVarianceError` looks for).
bool hasErrorContaining(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Group A: `out T` with T in input (contravariant) position — must be REJECTED
// ---------------------------------------------------------------------------

// §12.9 example A1: `interface IBad<out T> { method Consume(item: T); }`
// T appears as an ordinary method parameter — input position — which violates
// the `out` (covariant) annotation.
BEZEL_TEST(sema_variance_position, spec_12_9_out_in_input_param_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "interface IBad<out T> "
        "  method Consume(item: T); "
        "end IBad;");
    BEZEL_EXPECT_TRUE(hasVarianceError(r));
}

// §12.9: a second method parameter is also an input position.
BEZEL_TEST(sema_variance_position, spec_12_9_out_in_multi_param_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "interface IBad2<out T> "
        "  method Compare(a: T, b: T): int; "
        "end IBad2;");
    BEZEL_EXPECT_TRUE(hasVarianceError(r));
}

// §12.9: T in a read-write property type is an input position (has setter).
// A read-write property is NOT a pure output position.
BEZEL_TEST(sema_variance_position, spec_12_9_out_in_readwrite_property_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "interface IBad3<out T> "
        "  property Item: T; "
        "end IBad3;");
    BEZEL_EXPECT_TRUE(hasVarianceError(r));
}

// ---------------------------------------------------------------------------
// Group B: `in T` with T in output (covariant) position — must be REJECTED
// ---------------------------------------------------------------------------

// §12.9 example B1: `interface IBadIn<in T> { method Produce(): T; }`
// T appears as method return type — output position — violating `in`.
BEZEL_TEST(sema_variance_position, spec_12_9_in_in_return_type_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "interface IBadIn<in T> "
        "  method Produce(): T; "
        "end IBadIn;");
    BEZEL_EXPECT_TRUE(hasVarianceError(r));
}

// §12.9: T in a readonly property is a pure output position — violates `in`.
BEZEL_TEST(sema_variance_position, spec_12_9_in_in_readonly_property_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "interface IBadIn2<in T> "
        "  readonly property Current: T; "
        "end IBadIn2;");
    BEZEL_EXPECT_TRUE(hasVarianceError(r));
}

// ---------------------------------------------------------------------------
// Group C: `out T` in valid output positions — must be ACCEPTED
// ---------------------------------------------------------------------------

// §12.9 example C1: `interface IEnumerable<out T> { method GetEnumerator(): ... }`
// T in return type is a valid output position for `out`.
BEZEL_TEST(sema_variance_position, spec_12_9_out_in_return_type_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "interface IGoodOut<out T> "
        "  method Get(): T; "
        "end IGoodOut;");
    BEZEL_EXPECT_FALSE(hasVarianceError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §12.9: T in a readonly property — pure output — is valid for `out`.
BEZEL_TEST(sema_variance_position, spec_12_9_out_in_readonly_property_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "interface IGoodProp<out T> "
        "  readonly property Current: T; "
        "end IGoodProp;");
    BEZEL_EXPECT_FALSE(hasVarianceError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Group D: `in T` in valid input positions — must be ACCEPTED
// ---------------------------------------------------------------------------

// §12.9 example D1: `interface IComparer<in T> { method Compare(a: T, b: T): int; }`
// T in method parameters — input positions — is valid for `in`.
BEZEL_TEST(sema_variance_position, spec_12_9_in_in_params_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "interface IGoodIn<in T> "
        "  method Compare(a: T, b: T): int; "
        "end IGoodIn;");
    BEZEL_EXPECT_FALSE(hasVarianceError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §12.9: a single input parameter — valid for `in`.
BEZEL_TEST(sema_variance_position, spec_12_9_in_single_param_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "interface IAction<in T> "
        "  method Do(item: T); "
        "end IAction;");
    BEZEL_EXPECT_FALSE(hasVarianceError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Group E: Regression — existing patterns must continue to compile
// ---------------------------------------------------------------------------

// An invariant interface with T in both positions — always valid.
BEZEL_TEST(sema_variance_position, spec_12_9_invariant_both_positions_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "interface IList<T> "
        "  method Add(item: T); "
        "  method Get(): T; "
        "end IList;");
    BEZEL_EXPECT_FALSE(hasVarianceError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A non-generic interface is unaffected.
BEZEL_TEST(sema_variance_position, spec_12_9_non_generic_interface_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "interface IFoo "
        "  method DoSomething(); "
        "end IFoo;");
    BEZEL_EXPECT_FALSE(hasVarianceError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A class with an `out`-annotated type param is rejected semantically
// (issue #1559, Group F below) — classes are always invariant. Here we only
// confirm an ordinary invariant class generic compiles fine.
BEZEL_TEST(sema_variance_position, spec_12_9_class_invariant_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> "
        "  private _value: T?; "
        "  public method Get(): T? => _value; "
        "end Box;");
    BEZEL_EXPECT_FALSE(hasVarianceError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §12.9: two separate interfaces, one `out` (IProducer) and one `in`
// (IConsumer), each with T in the correct position — both must be accepted.
BEZEL_TEST(sema_variance_position, spec_12_9_producer_consumer_pair_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "interface IProducer<out T> "
        "  method Produce(): T; "
        "end IProducer; "
        "interface IConsumer<in T> "
        "  method Consume(item: T); "
        "end IConsumer;");
    BEZEL_EXPECT_FALSE(hasVarianceError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §12.9: IEvent<out T> canonical example from the spec. T appears inside
// the parameter of a function type `(T) -> ()` which is itself a parameter of
// Subscribe — contravariant-of-contravariant = covariant position overall.
// This composed position check is deferred (nested), so it must be ACCEPTED
// (no false positive).
BEZEL_TEST(sema_variance_position, spec_12_9_event_out_T_nested_function_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "interface IEvent<out T> "
        "  method Subscribe(handler: (T) -> ()); "
        "end IEvent;");
    // Nested position composition is deferred — must not false-positive.
    BEZEL_EXPECT_FALSE(hasVarianceError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Group F (issue #1559, §12.9 v0.181): variance markers are interface-only
// syntax. `in`/`out` on a class, record, enum, method, or type-alias generic
// parameter is a compile error — the shared `GenericParam` grammar (Appendix
// A.4) admits the marker everywhere, but outside an interface's own type
// parameters it must be rejected semantically.
// ---------------------------------------------------------------------------

// §12.9: `class Box<out T>` — the spec's own counter-example — is rejected.
BEZEL_TEST(sema_variance_marker_scope, spec_12_9_class_out_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<out T> "
        "  private _v: T; "
        "  public method Get(): T => _v; "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "12.9"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "class type parameter"));
}

// §12.9: `in` is rejected on a class parameter exactly like `out`.
BEZEL_TEST(sema_variance_marker_scope, spec_12_9_class_in_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Sink<in T> "
        "  public method Consume(item: T) end Consume; "
        "end Sink;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "12.9"));
}

// §12.9: a record's own type parameter may not be marked variant either.
BEZEL_TEST(sema_variance_marker_scope, spec_12_9_record_out_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "record Pair<out T>(a: T, b: T);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "12.9"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "record type parameter"));
}

// §12.9: a method's own type parameter is never variant, even when the
// method sits on an ordinary (non-generic) class.
BEZEL_TEST(sema_variance_marker_scope, spec_12_9_method_out_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Util "
        "  public static method Identity<out T>(x: T): T => x; "
        "end Util;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "12.9"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "method type parameter"));
}

// §12.9: a method's own type parameter is rejected even INSIDE an interface
// — the position rules govern the interface's own parameters, not a
// method's (the interface's own `out T` on IHost is fine; the method-level
// `out U` on MakeHandler is not).
BEZEL_TEST(sema_variance_marker_scope, spec_12_9_method_out_rejected_inside_interface) {
    auto r = parseSemaAll(
        "module M; "
        "interface IHost<out T> "
        "  method MakeHandler<out U>(seed: U): T; "
        "end IHost;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "12.9"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "method type parameter"));
}

// §12.9 / §27.8: a type alias's own type parameter may not be marked
// variant.
BEZEL_TEST(sema_variance_marker_scope, spec_12_9_type_alias_out_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> "
        "  private _v: T; "
        "  public method Get(): T => _v; "
        "end Box; "
        "type Alias<out T> = Box<T>;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "12.9"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "type alias parameter"));
}

// ---------------------------------------------------------------------------
// Group G (issue #1559, §11.8 v0.181): an interface that declares a static
// member may not mark its type parameters variant either — variance is
// defined by instance-reference assignability, and static signatures have
// no recorded reading.
// ---------------------------------------------------------------------------

// §11.8 worked example: `INumber<T>` declares `static method Zero(): T;` —
// marking T covariant is rejected even though `Zero` alone is a pure output
// position (the §12.9 position check would accept it; §11.8 forbids the
// marker outright once any static member is present).
BEZEL_TEST(sema_variance_marker_scope, spec_11_8_static_method_out_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "interface INumber<out T> where T: INumber<T> "
        "  static method Zero(): T; "
        "end INumber;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "11.8"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "static member"));
}

// §11.8: an operator overload is implicitly static (A.4 `OperatorDecl ::=
// Visibility "static" "method" "operator" ...`), so it triggers the same
// rejection as an explicit `static method`.
BEZEL_TEST(sema_variance_marker_scope, spec_11_8_static_operator_out_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "interface INumber2<out T> where T: INumber2<T> "
        "  static operator +(a: T, b: T): T; "
        "end INumber2;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "11.8"));
}

// §11.8 regression: an interface with a static member but an INVARIANT type
// parameter (no `in`/`out`) is unaffected — only the marker is forbidden.
BEZEL_TEST(sema_variance_marker_scope, spec_11_8_static_method_invariant_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "interface INumber3<T> where T: INumber3<T> "
        "  static method Zero(): T; "
        "end INumber3;");
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, "11.8"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §12.9 / §11.8 positive control (issue #1559): an interface with ONLY
// instance members and a variant parameter still compiles — the rejection
// is scoped to (a) non-interface declarations and (b) interfaces that
// declare a static member, not to interface variance in general.
BEZEL_TEST(sema_variance_marker_scope, spec_12_9_interface_instance_only_variance_still_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "interface IProducer<out T> "
        "  method Produce(): T; "
        "end IProducer;");
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, "12.9"));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, "11.8"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
