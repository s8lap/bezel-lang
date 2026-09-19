// fix #4351 / spec §15.1 *Inline arrays* (v0.307).
//
// §15.1 verbatim: "For non-nullable class-typed elements (`[N]C` without
// the `owned` slot marker), the elements stored in the array are *borrows*
// — the initializer expressions must be borrows of class instances already
// owned elsewhere, and those owners must outlive the inline array." The
// owning form `[N owned]C` (§15.1.1, fixed by #4336 / `569a680`) takes
// ownership and so admits a `new T(...)` element; the non-owning form
// does not.
//
// ROOT CAUSE (traced, `ExprTC::checkCollectionLiteralElementAssignability`,
// ExprTypecheck.cpp, the `InlineArray` arm). The check walks every element
// through `checkAssignable`, which only validates the *type* relationship
// (a `Widget` is assignable to `[1]IHolder` because Widget implements
// IHolder) — it does NOT consult §15.1's borrow-only contract. A `new
// Widget(...)` element therefore compiles clean and the resulting array
// silently leaks (the leak #4336 measured on the OWNING twin: 4800 B / 200
// objs on `Widget("init")`, plus 4776 B / 199 on the displaced
// `Widget("hello")` after overwrite). The leak is the §15.1 violation
// made visible — `new T(...)` is a fresh producer with no outer owner, the
// temporary is disposed at end-of-statement, the slot holds a dangling
// borrow from birth.
//
// THE FIX is one new sema rejection at the `InlineArray` arm of
// `checkCollectionLiteralElementAssignability` (the single funnel point
// that already serves LocalDecl / FieldDecl / PropertyDecl / ReturnStmt /
// default-param-value / call-argument). When the target is non-owning
// (`!target->arrayOwned`) AND the element type is class-typed
// (Named + Class or Interface declaration), each element that is a
// `new T(...)` expression is rejected with a §15.1 diagnostic. The
// existing per-element `checkAssignable` for type compatibility stays
// — a non-conforming class at the same slot additionally gets the
// §5.1/§11.2 message, and an owning `[N owned]T` slot keeps its
// post-#4336 acceptance.
//
// SCOPE (intentionally narrow — this lane fixes ONLY the spec-strict
// sema-side rejection the issue filed for):
//   - `new T(...)` element ONLY. A `someOwnedVar` identifier is a borrow
//     and stays admissible — the spec rule rejects FRESH producers, not
//     borrows. Owned-returning calls (`[Factory()]`) are equivalent to
//     `new` semantically and would be filed as a separate ruling.
//   - Class-typed element ONLY: Named with Class or Interface
//     declaration. Primitives (`[1]int := [42]`), records
//     (`[1]Point := [new Point(0,0)]`), enums, choices, and
//     runtime-managed types (`string`, function types) are NOT in scope —
//     §15.1's "class-typed elements" wording scopes this check.
//   - The existing §15.4 BORROWING-FAMILY reject for `Array<T>` /
//     `Set<T>` / … (per `checkBorrowingFamilyLiteralElements`) is
//     unaffected; the §15.1 inline-array check is the SIMPLE-non-owning
//     twin of §15.4's owning-vs-borrowing-family question, applied at a
//     different target shape. An existing `[1 owned]IHolder := [new
//     Widget(...)]` row stays ACCEPTED through this fix.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The §15.1 borrow-only-contract diagnostic. Pinned on the wording — not
// just `hasErrors()` — so a branch-order change that routes the reject to
// a catch-all would still flip `hasErrors()` but would miss the specific
// §15.1 wording the rule requires.
bool hasNonOwningNewRejected(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("non-owning inline array") != std::string::npos &&
            d.message.find("15.1") != std::string::npos &&
            d.message.find("borrow of a class instance") != std::string::npos)
            return true;
    return false;
}

// Shared prelude: an interface `IHolder`, a CONFORMING concrete class
// `Widget` (lists `IHolder` and matches its members), a NON-conforming
// concrete class `WidgetX` (no interface at all), a record `Point` (NOT
// class-typed — record elements must NOT trigger the §15.1 check), and a
// non-class type `Helper` for value-typed cases.
const std::string kPrelude =
    "module M; "
    "interface IHolder method GetV(): string; end IHolder; "
    "class Widget: IHolder "
    "  private _s: string; "
    "  public constructor(s: string) _s := s; end constructor; "
    "  public method GetV(): string return _s; end GetV; "
    "end Widget; "
    "class PlainClass "
    "  public X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "end PlainClass; "
    "record Point(x: int, y: int); ";

JitResult run(const std::string& mainBody,
              const std::string& extraMember = "") {
    return runAot(kPrelude +
                  "class C public static method Main(): int " +
                  mainBody + " end Main; " + extraMember + " end C;");
}

}  // namespace

// ============================================================================
// REJECT ROWS — the spec-strict §15.1 violation.
// ============================================================================

// Row (1) — the issue's literal repro: an interface element, `new Widget(...)`
// at a NON-OWNING inline-array slot. Pre-fix: compiles clean and leaks
// (4800 B / 200 objs on the `Widget("init")`, measured on HEAD before
// `569a680`; the non-owning shape's leak is #4351's measured repro).
// Post-fix: clean §15.1 reject.
BEZEL_TEST(inline_array_non_owning_new_rejected_4351,
           interface_element_new_rejected_spec_15_1) {
    auto r = run(
        "var outer: [1]IHolder := [new Widget(\"init\")]; "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNonOwningNewRejected(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Row (2) — a CONCRETE class element. The spec's "class-typed elements"
// wording includes concrete classes, and the brief asks for the rule to
// fire there too. `PlainClass` is a class with no Dispose and no owned
// fields (trivial-dispose), proving the rejection is keyed on
// CLASS-TYPED element type, not on non-trivial-dispose.
BEZEL_TEST(inline_array_non_owning_new_rejected_4351,
           concrete_class_element_new_rejected_spec_15_1) {
    auto r = run(
        "var outer: [1]PlainClass := [new PlainClass(7)]; "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNonOwningNewRejected(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The §15.1 rule fires at EVERY collection-literal initializer site. The
// fix lives in the single funnel point `checkCollectionLiteralElementAssignability`
// which is called from LocalDecl, FieldDecl, PropertyDecl, ReturnStmt,
// default-param-value, and call-argument — verifying each position keeps
// the rejection uniform.

// Row (3) — FIELD initializer (a `FieldDecl`'s initializer slot, distinct
// AST node from `LocalDecl`, reached through the `Member` visitor arm).
BEZEL_TEST(inline_array_non_owning_new_rejected_4351,
           field_initializer_new_rejected_spec_15_1) {
    auto r = runAot(
        kPrelude +
        "class C "
        "  public F: [1]IHolder := [new Widget(\"init\")]; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNonOwningNewRejected(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Row (4) — RETURN statement. The collection-literal-as-return-value
// position reaches the same `checkCollectionLiteralElementAssignability`
// funnel from `visitReturnStmt` (ExprTypecheck.cpp:~14945), which already
// runs the #4310 per-element type check.
BEZEL_TEST(inline_array_non_owning_new_rejected_4351,
           return_stmt_new_rejected_spec_15_1) {
    auto r = run(
        "return Make();",
        /*extraMember=*/
        "public static method Make(): [1]IHolder "
        "  return [new Widget(\"init\")]; "
        "end Make;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNonOwningNewRejected(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Row (5) — NESTED literal at a NON-OWNING inline-array slot. `[1][1]IHolder`
// is an inline array whose element type is itself an inline array of an
// interface — the outer `checkCollectionLiteralElementAssignability`
// recurses into the inner literal and re-applies the §15.1 check.
BEZEL_TEST(inline_array_non_owning_new_rejected_4351,
           nested_literal_new_rejected_spec_15_1) {
    auto r = run(
        "var outer: [1][1]IHolder := [[new Widget(\"init\")]]; "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNonOwningNewRejected(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Row (6) — DEFAULT parameter value (`method M(p: T := [...])`). The
// default-value slot is one of §17.7's written-declared-type positions;
// the wiring #4332 closed for the §15.4 admissibility gate runs the SAME
// `checkCollectionLiteralElementAssignability` per-element walk.
BEZEL_TEST(inline_array_non_owning_new_rejected_4351,
           default_param_value_new_rejected_spec_15_1) {
    auto r = run(
        "return Foo();",
        /*extraMember=*/
        "public static method Foo(p: [1]IHolder := [new Widget(\"init\")]): int "
        "  return 0; "
        "end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNonOwningNewRejected(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ============================================================================
// CONTROL ROWS — what stays ACCEPTED.
// ============================================================================

// Row (7) — OWNING form: `[1 owned]IHolder := [new Widget(...)]` is the
// spec-conformant shape #4336 / `569a680` already disposes correctly. The
// §15.1 borrow-only-contract check keys on `!arrayOwned`, so the owning
// form is exempt — this row MUST stay green.
BEZEL_TEST(inline_array_non_owning_new_rejected_4351,
           owning_form_new_accepted_control_spec_15_1_1) {
    auto r = runAot(
        kPrelude +
        "class C "
        "  public static method Main(): int "
        "    var outer: [1 owned]IHolder := [new Widget(\"init\")]; "
        "    return outer[0].GetV().Length; "
        "  end Main; "
        "end C;");
    // Pre-#4336 there was a leak here; the post-#4336 expectation is
    // clean compile + clean run, no diagnostics at all.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);  // "init".Length == 4
}

// Row (8) — LEGITIMATE BORROW: `[1]IHolder := existingOwnedHolder` is
// the spec-stated correct shape — an identifier is a borrow of an owned
// value, satisfying §15.1's "borrows of class instances already owned
// elsewhere" clause. The §15.1 check fires only on `new` expressions,
// not on plain identifiers, so this row stays ACCEPTED.
BEZEL_TEST(inline_array_non_owning_new_rejected_4351,
           legitimate_borrow_accepted_control_spec_15_1) {
    auto r = runAot(
        kPrelude +
        "class C "
        "  public static method Main(): int "
        "    using var w := new Widget(\"init\"); "
        "    var outer: [1]IHolder := [w]; "
        "    return outer[0].GetV().Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}

// Row (9) — VALUE-TYPED element: `[1]Point := [new Point(0, 0)]` is the
// spec's EXAMPLE for a record-typed element (the example at §15.1 line
// 2874: `var pt: [2]Point := [new Point(0, 0), new Point(1, 1)];`).
// Records are value-typed, NOT class-typed, so the §15.1 borrow-only
// check does NOT fire — this row MUST stay green.
BEZEL_TEST(inline_array_non_owning_new_rejected_4351,
           value_typed_record_element_new_accepted_control_spec_15_1) {
    auto r = runAot(
        kPrelude +
        "class C "
        "  public static method Main(): int "
        "    var pt: [2]Point := [new Point(0, 0), new Point(1, 1)]; "
        "    return pt[0].x + pt[1].x; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// Row (10) — PRIMITIVE element: `[1]int := [42]` — primitives are NOT
// class-typed, so the §15.1 check does NOT fire. Sanity row: confirms
// the predicate's class-typed arm filters out everything except Class /
// Interface declaration kinds.
BEZEL_TEST(inline_array_non_owning_new_rejected_4351,
           primitive_element_literal_accepted_control_spec_15_1) {
    auto r = runAot(
        kPrelude +
        "class C "
        "  public static method Main(): int "
        "    var xs: [3]int := [1, 2, 3]; "
        "    return xs[0] + xs[1] + xs[2]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}