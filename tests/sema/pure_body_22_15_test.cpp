// Spec-derived tests for the `pure` member modifier — Slice 2: the closed
// body rule set.
//
// Spec §22.15 "Pure members" (v0.202, proposal 0042) — "The pure body" +
// "The throw carve-out" + "Pure constructors" + Appendix C.14 bullet 3.
//
// The closed body rule set enumerates exactly what a pure body may contain
// (rule 1: reads; rule 2: frame-local mutation; rule 3: value/record/enum/
// string operations; rule 4: defining lambdas + method references; rule 5:
// control flow + Range counted-loop `for`; rule 6: calls to pure members;
// rule 7: throw with the construction carve-out). Everything outside is
// rejected.
//
// Slice 2 covers the load-bearing closed-body rules:
//   * frame-local mutation root rule (AssignmentStmt + IncrementStmt)
//   * calls to non-pure members (rule 6)
//   * class construction outside the throw carve-out
//   * throw carve-out (`new E(args)` iff E trivial-dispose + pure ctor)
//   * consuming discard
//   * IEnumerable-source `for` rejection
//   * `using var` (sync) rejection (disposal-running)
//   * `await` rejection
//   * birth-store rule for pure constructors
//
// Implicit classification (auto-getter + record synthesis), dispatch
// matching (override rules), and field-initializer checking are later
// slices.

#include "sema/ExprTypecheck.hpp"
#include "sema/PureChecker.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
        bezel::sema::checkPureBodies(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// =========================================================================
// §22.15 rule 2: frame-local mutation root rule.
// =========================================================================

// §22.15 rule 2: assignment to a local variable — admitted.
BEZEL_TEST(pure_body_22_15, pure_local_assignment_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure method M(): int "
        "    var x: int := 0; "
        "    x := x + 1; "
        "    return x; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15 rule 2: compound assignment to a local — admitted.
BEZEL_TEST(pure_body_22_15, pure_local_compound_assignment_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure method M(): int "
        "    var x: int := 1; "
        "    x += 5; "
        "    return x; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15 rule 2: increment of a local — admitted.
BEZEL_TEST(pure_body_22_15, pure_local_increment_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure method M(): int "
        "    var x: int := 1; "
        "    x++; "
        "    return x; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15 contrapositive: assignment to an instance field of `this` in a
// pure METHOD (not a constructor) — rejected.
BEZEL_TEST(pure_body_22_15, pure_method_writes_field_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _v: int := 0; "
        "  public pure method M(): int "
        "    _v := 1; "
        "    return _v; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "assignment is not admitted in a pure body"));
}

// §22.15 contrapositive: assignment to a static field — rejected.
BEZEL_TEST(pure_body_22_15, pure_writes_static_field_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static _s: int := 0; "
        "  public pure method M(): int "
        "    _s := 1; "
        "    return _s; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "assignment is not admitted in a pure body"));
}

// =========================================================================
// §22.15 contrapositive: the consuming discard; `using var`; `await`.
// =========================================================================

// §22.15 contrapositive: the consuming discard `_ := expr;` is rejected.
BEZEL_TEST(pure_body_22_15, pure_discard_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method Other(): int => 0; "
        "  public pure method M(): int "
        "    _ := Other(); "
        "    return 0; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "consuming discard"));
}

// §22.15 contrapositive: `using var` is disposal-running.
BEZEL_TEST(pure_body_22_15, pure_using_var_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  destructor end destructor; "
        "  public pure method M(c: C): int "
        "    using var x: C := c; "
        "    return 0; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "`using var`"));
}

// =========================================================================
// §22.15 rule 5: `for`-source admission — a concrete Array<T> source is
// the normative elision and is ADMITTED; a Range source is admitted; an
// interface-typed source, and an Array<T> whose element is a type
// parameter, remain rejected.
// =========================================================================

// §22.15 rule 5: a Range `for` loop — admitted (the normative form inside
// a pure body).
BEZEL_TEST(pure_body_22_15, pure_range_for_loop_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure method M(): int "
        "    var sum: int := 0; "
        "    for var i in 0..<10 do sum := sum + i; end; "
        "    return sum; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15 rule 5: a `for` over a CONCRETE Array<T> source uses direct
// indexed loop (normative elision); accepted in pure bodies.
// (A non-concrete Array<T> — with a type-parameter element — is rejected.)
BEZEL_TEST(pure_body_22_15, pure_concrete_array_for_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure method M(xs: Array<int>): int "
        "    var sum: int := 0; "
        "    for var x in xs do sum := sum + x; end; "
        "    return sum; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15 rule 5, the boundary the concrete-Array admission stops at: "A
// source whose static type is an interface (`IEnumerable<T>` included)
// or a generic type parameter — even one instantiated at a concrete
// sequence type — remains rejected."  An interface-typed source has no
// concrete layout for the elision to read, so the `for` desugaring
// constructs an enumerator and the loop is rejected.
BEZEL_TEST(pure_body_22_15, pure_interface_source_for_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure method M(xs: IEnumerable<int>): int "
        "    var sum: int := 0; "
        "    for var x in xs do sum := sum + x; end; "
        "    return sum; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "`for` loop source"));
}

// §22.15 rule 5, the same clause's other half: a `for` over an
// `Array<T>` whose ELEMENT is a type parameter is not a concrete
// `Array<T>` at this body's own check site — the element's layout is
// only known at the instantiation site, so the elision does not apply
// and the source stays rejected.
BEZEL_TEST(pure_body_22_15, pure_type_param_element_array_for_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C<E> "
        "  public pure method M(xs: Array<E>): int "
        "    var sum: int := 0; "
        "    for var x in xs do sum := sum + 1; end; "
        "    return sum; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "`for` loop source"));
}

// §22.15 rule 5, the elision clause's THIRD concrete type: a `string`
// source. Verbatim: "over a source whose static type at this body's own
// check site is concretely `Array<T>`, `OwningArray<T>`, class-typed
// `OwningBuffer<T>`, or `string`, §29.2's admitted protocol elision is
// likewise the normative semantics inside a pure body: the compiler emits
// the direct indexed loop — a bounds read and per-element reads, rule-1
// material — and constructs no enumerator."
//
// The clause's stated reason is that "`string.Length` and the `string`
// indexer (stdlib §9.6) ... are `pure`-declared, so for those reads either
// lowering satisfies the fragment".
//
// FAILS-WITHOUT / PASSES-WITH: before the fix `isEnumeratorConstructingSource`
// listed `string` among the enumerator-constructing sources, so this program
// was rejected with the "`for` loop source" diagnostic.
BEZEL_TEST(pure_body_22_15, pure_string_source_for_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure method M(s: string): int "
        "    var t: int := 0; "
        "    for var ch in s do t := t + int(ch); end; "
        "    return t; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasMsg(r, "`for` loop source"));
    // The loop variable is a VALUE COPY typed `char` (§22.15 rule 5's
    // "a value copy for value elements and `string`"; §29.2's table gives
    // `ReadOnlyStringSpan` the same `char` shape). Reading it at its own type
    // is what makes this row load-bearing on the iteration-variable typing
    // path rather than on the admission alone: an admit-with-no-typed-element
    // would still fail `int(ch)` at the operator.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15 rule 5, the elision clause's FOURTH concrete type: class-typed
// `OwningBuffer<T>`. Same clause, verbatim (as above): "class-typed
// `OwningBuffer<T>`", with the reason "`OwningBuffer<T>.Length` (stdlib
// §4.14) is `pure`-declared as an immutable-once-allocated extent read".
//
// stdlib §4.14 declares the class-typed presentation as
// `class OwningBuffer<T>: IEnumerable<T?>` — it IS an enumerator-constructing
// type, admitted only via the elision. §29.2's table note 5 says so
// directly: "For class-typed `OwningBuffer<T>`, the type implements
// `IEnumerable<T?>` and the standard enumerator path applies, with the
// elision rule below."
//
// This row also pins that the fix does not accidentally admit the wrong
// element category: the VALUE-typed presentation is admitted on the
// DIFFERENT ground §22.15 states for it ("over ... value-typed
// `OwningBuffer<T>` the desugaring is ALREADY the direct indexed loop"), and
// both are checked here because the two presentations are one generic class.
BEZEL_TEST(pure_body_22_15, pure_class_typed_owning_buffer_for_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class Dog "
        "  public constructor() end constructor; "
        "end Dog; "
        "class C "
        "  public pure method M(b: OwningBuffer<Dog>): int "
        "    var n: int := 0; "
        "    for var d in b do n := n + 1; end; "
        "    return n; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasMsg(r, "`for` loop source"));
}

// The value-typed OwningBuffer<T> presentation, the clause's other
// OwningBuffer arm ("over ... value-typed `OwningBuffer<T>` the desugaring
// is already the direct indexed loop"). Paired with the row above so a
// future edit cannot admit one presentation and reject the other without a
// row going red.
BEZEL_TEST(pure_body_22_15, pure_value_typed_owning_buffer_for_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure method M(b: OwningBuffer<int>): int "
        "    var n: int := 0; "
        "    for var v in b do n := n + 1; end; "
        "    return n; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasMsg(r, "`for` loop source"));
}

// The elision must NOT widen the diagnostic's own stated reasons: the two
// rejections the clause keeps (`IEnumerable<T>`-typed, and `Array<T>` whose
// ELEMENT is a type parameter) must still fire after the fix, and the
// message must no longer name `string` as one of them. Pins the text so a
// later re-widening of either the arm list or the message is caught.
BEZEL_TEST(pure_body_22_15, pure_for_source_diagnostic_text_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure method M(xs: IEnumerable<int>): int "
        "    var sum: int := 0; "
        "    for var x in xs do sum := sum + 1; end; "
        "    return sum; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "`for` loop source"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "IEnumerable<T>-typed source"));
    // `string` is admitted by the clause, so the diagnostic must not list it
    // as a reason for rejection.
    BEZEL_EXPECT_FALSE(hasMsg(r, "a `string` source"));
}

// =========================================================================
// §22.15 contrapositive: class construction outside throw; the throw
// carve-out.
// =========================================================================

// §22.15 contrapositive: a `new C()` outside throw — rejected.
BEZEL_TEST(pure_body_22_15, pure_new_class_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  destructor end destructor; "
        "  public pure method M(): int "
        "    var x: C := new C(); "
        "    return 0; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "class construction"));
}

// §22.15 throw carve-out: `throw new ArgumentException(...)` is admitted
// when the constructor is pure (built-in exceptions' ()/(message) ctors
// are pure per §22.15 "Intrinsic purity").
BEZEL_TEST(pure_body_22_15, pure_throw_built_in_exception_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure method Check(x: int): int "
        "    if x < 0 then "
        "      throw new ArgumentException(\"x must not be negative\"); "
        "    end; "
        "    return x; "
        "  end Check; "
        "end C;");
    // Built-in exception types — Slice 2 conservatively admits the throw
    // (see throwCarveOutAdmits). Constructor-purity propagation for the
    // stdlib exception types is the Slice 4 (stdlib wave) territory.
    BEZEL_EXPECT_FALSE(hasMsg(r, "throw carve-out"));
}

// =========================================================================
// §22.15 rule 6: calls to non-pure members are rejected.
// =========================================================================

// §22.15 rule 6: a call to a non-pure method on `this` is rejected.
BEZEL_TEST(pure_body_22_15, pure_calls_impure_method_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _v: int := 0; "
        "  public method Bump(): int "
        "    _v := _v + 1; "
        "    return _v; "
        "  end Bump; "
        "  public pure method M(): int "
        "    return Bump(); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "is not pure-declared"));
}

// §22.15 rule 6: a call to ANOTHER pure method — admitted.
BEZEL_TEST(pure_body_22_15, pure_calls_pure_method_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure method Double(x: int): int => x * 2; "
        "  public pure method M(): int => Double(3); "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =========================================================================
// §22.15 "Pure constructors": the birth-store rule.
// =========================================================================

// §22.15 birth-store: a pure constructor may assign fields of `this`.
BEZEL_TEST(pure_body_22_15, pure_ctor_birth_store_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _v: int; "
        "  public pure constructor(v: int) "
        "    _v := v; "
        "  end constructor; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15 birth-store rule: even in a pure ctor, writing to a STATIC field
// is rejected.
BEZEL_TEST(pure_body_22_15, pure_ctor_writes_static_field_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static _s: int := 0; "
        "  public pure constructor() "
        "    _s := 1; "
        "  end constructor; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "assignment is not admitted in a pure body"));
}

// §22.15 "Pure constructors" (#2061, item 1): a pure constructor's
// `base(...)`/`this(...)` chain target must itself be pure. Implemented in
// PureChecker::checkPureCtorChainPurity; regression tests live in
// pure_residuals_2061_test.cpp.
