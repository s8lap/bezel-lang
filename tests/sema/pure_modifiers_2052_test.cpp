// Spec-derived tests for the `pure` member modifier — Slice 1: grammar,
// placement restrictions, and signature restrictions. The pure BODY rule
// set (closed-body, throw carve-out, pure constructors, implicit
// classification, dispatch matching) is covered in later slices.
//
// Spec §22.15 "Pure members" (v0.202, proposal 0042); Appendix C.14 is the
// test checklist. Cite §22.15 throughout.
//
// Slice 1 covers Appendix C.14's first three bullets:
//   * "`pure` combines with neither `async` nor `yield`; destructors, static
//     constructors, `extern` declarations, and auto-properties carry no
//     `pure`."
//   * "A pure member declares no `ref`/`out`/`out owned`/`owned`/`scoped`
//     parameter and no `owned` class-typed return."
//   * (Re: placement on getter vs set) "Purity attaches to the getter; `pure`
//     on `set` is always rejected."
//
// Each test names the spec sub-section it derives from. The body-rule
// checks (Slice 2+) live in pure_checker_spec_22_15_*.cpp companions.

#include "sema/ExprTypecheck.hpp"
#include "sema/PureChecker.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

// fix #3985: delegates to the shared gated helper (sema_support.hpp)
// instead of duplicating the parse-error-gate logic locally.
bezel::test::SemaResult parseSemaAll(std::string_view src) {
    return bezel::test::parseAndCheckPureGated(src);
}

// True iff some reported diagnostic's message contains `needle`.
bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// =========================================================================
// §22.15 "Where `pure` may appear": a `pure` instance method compiles.
// =========================================================================
BEZEL_TEST(pure_modifiers_2052, pure_instance_method_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure method Id(x: int): int => x; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15: a `pure static method compiles.
BEZEL_TEST(pure_modifiers_2052, pure_static_method_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static pure method Zero(): int => 0; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15 example: the spec's `Checked` validation idiom — a pure static
// method whose body reads parameters and throws. The throw carve-out
// (Slice 2) admits the constructor; this slice accepts the signature.
BEZEL_TEST(pure_modifiers_2052, pure_checked_signature_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static pure method Checked(x: int, low: int, high: int): int "
        "    return (if x < low then low else (if x > high then high else x)); "
        "  end Checked; "
        "end C;");
    // Body-rule check is Slice 2; the signature is fine.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15 "Purity attaches to the getter": an expression-bodied property
// takes property-level `pure` and compiles.
BEZEL_TEST(pure_modifiers_2052, pure_expression_property_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _v: int := 0; "
        "  public pure property V: int => _v; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15 example: the `Account.Balance` read-write block-form property
// uses per-accessor `pure get` — accepted here. The setter has no `pure`.
BEZEL_TEST(pure_modifiers_2052, pure_get_accessor_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class Account "
        "  private _balance: int := 0; "
        "  public property Balance: int "
        "    pure get => _balance; "
        "    set => _balance := value; "
        "  end Balance; "
        "end Account;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15: a pure indexer getter (expression-bodied; the `readonly` keyword
// has been removed in v0.214 per §23.2 — it no longer combines with an
// expression body, since the body already expresses read-onlyness).
BEZEL_TEST(pure_modifiers_2052, pure_indexer_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class Bag "
        "  private _v: int := 0; "
        "  public pure property this[i: int]: int => _v; "
        "end Bag;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15: a pure interface method (abstract signature).
BEZEL_TEST(pure_modifiers_2052, pure_interface_method_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "interface I "
        "  pure method Cost(): int; "
        "end I;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15: a pure static interface method (abstract signature).
BEZEL_TEST(pure_modifiers_2052, pure_static_interface_method_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "interface I "
        "  static pure method Zero(): int; "
        "end I;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15: a pure interface property.
BEZEL_TEST(pure_modifiers_2052, pure_interface_property_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "interface I "
        "  pure readonly property Count: int; "
        "end I;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =========================================================================
// §22.15 "Where `pure` is rejected" — Appendix C.14 bullet 1.
// =========================================================================

// §22.15: `pure` and `async` are mutually exclusive on a method.
BEZEL_TEST(pure_modifiers_2052, pure_async_method_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public async pure method M(): Task   yield return 1; end M; "
        "end C;");
    // The async+pure combination is rejected even before body checks.
    BEZEL_EXPECT_TRUE(hasMsg(r, "combines with neither `async` nor `yield`"));
}

// §22.15: `pure` on an event is rejected (events are the synchronization
// surface purity fences off).
BEZEL_TEST(pure_modifiers_2052, pure_event_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure event E: int; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "`pure` is not admitted on an event"));
}

// §22.15: `pure` on an auto-property declaration is rejected as redundant
// (the auto-getter is implicitly pure-classified).
BEZEL_TEST(pure_modifiers_2052, pure_auto_property_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure property V: int := 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "`pure` on an auto-property is redundant"));
}

// §22.15: property-level `pure` is rejected on a read-write block-form
// property — the per-accessor `pure get` form is required.
BEZEL_TEST(pure_modifiers_2052, pure_property_level_on_read_write_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _v: int := 0; "
        "  public pure property V: int "
        "    get => _v; "
        "    set => _v := value; "
        "  end V; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "property-level `pure` marks the single getter"));
}

// §22.15: `pure set` is always rejected — purity attaches to the getter
// only.
BEZEL_TEST(pure_modifiers_2052, pure_set_accessor_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _v: int := 0; "
        "  public property V: int "
        "    get => _v; "
        "    pure set => _v := value; "
        "  end V; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "`pure` on a `set` accessor is always rejected"));
}

// §22.15: `pure` on a static constructor is rejected (no call site exists
// to consume the fact).
BEZEL_TEST(pure_modifiers_2052, pure_static_constructor_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  static pure constructor() end constructor; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "`pure` is not admitted on a static constructor"));
}

// §22.15: `pure` on a field is rejected (the auto-accessor classification
// is implicit; fields are not members that can carry a purity promise).
BEZEL_TEST(pure_modifiers_2052, pure_field_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure v: int := 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "`pure` is not admitted on a field"));
}

// =========================================================================
// §22.15 "Parameters and result" — Appendix C.14 bullet 2.
// =========================================================================

// §22.15: a pure method declares no `ref` parameter.
BEZEL_TEST(pure_modifiers_2052, pure_ref_param_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure method M(ref x: int): int => x; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "a `pure` member declares no"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "`ref`"));
}

// §22.15: a pure method declares no `out` parameter.
BEZEL_TEST(pure_modifiers_2052, pure_out_param_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure method M(out x: int): int "
        "    x := 1; return x; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "a `pure` member declares no"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "`out`"));
}

// §22.15: a pure method declares no `owned` parameter.
BEZEL_TEST(pure_modifiers_2052, pure_owned_param_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure method M(owned x: C): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "a `pure` member declares no"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "`owned`"));
}

// §22.15: a pure method declares no `scoped` parameter. (`scoped` is the
// storage-bounded callback parameter; a pure body can never exercise it.)
BEZEL_TEST(pure_modifiers_2052, pure_scoped_param_rejected_spec_22_15) {
    // `scoped` on a class/interface-typed parameter is the v0.180 form
    // (#1439). Here `scoped x: C` is the relevant shape.
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method Get(): int => 0; "
        "  public pure method M(scoped x: C): int => x.Get(); "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "a `pure` member declares no"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "`scoped`"));
}

// §22.15: a pure method declares no `owned` CLASS-typed return. (Pure
// bodies cannot produce owned class instances; the declaration would merely
// defer the error into the body.)
BEZEL_TEST(pure_modifiers_2052, pure_owned_class_return_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static pure method Factory(): owned C => new C(); "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "no `owned` class-typed return"));
}

// §22.15 (control): a pure method may return a record value — the
// `pure`+`owned`-class-return check (§22.15) fires only for a `retOwned &&
// isUserClassTypeRef` return (TypeResolution_Checks.cpp ~12201), which a
// plain record return never satisfies.
//
// fix #3985: the original fixture wrote `owned R` on the record return to
// exercise that non-firing directly, but `owned` is unconditionally
// rejected on ANY record return type by an earlier, unrelated §28.1 check
// ("`owned` requires a class type, an interface type, or a bare type
// parameter") regardless of `pure` — verified directly against the real
// `bezelc -c`. Keeping `owned R` here would make the fixture fail to
// compile for a reason that has nothing to do with what this test names,
// so the corrected fixture drops the (always-invalid) `owned` marker: a
// plain record return, which compiles cleanly and — because `retOwned` is
// false — still never reaches the class-typed-return check this test's
// name and assertion are about.
BEZEL_TEST(pure_modifiers_2052, pure_record_return_not_flagged_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "record R(v: int); "
        "class C "
        "  public static pure method Make(): R => new R(0); "
        "end C;");
    // fix #3997: an absence-only claim on ONE diagnostic substring also
    // holds when the fixture never parsed at all (gated by `parseSemaAll`
    // -> `parseAndCheckPureGated`). `!r.diagnostics.hasErrors()` is a real
    // discriminator: the gate makes "parsed+checked clean" and "gated off
    // by a parse error" mutually exclusive.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // No "owned class-typed return" diagnostic should fire.
    BEZEL_EXPECT_FALSE(hasMsg(r, "no `owned` class-typed return"));
}

// §22.15: a pure constructor — accepted at the signature level (the body
// birth-store rule and field-initializer check are Slice 2). A class with
// no `owned` fields and no destructor is trivial-dispose; pure-ctor on a
// non-trivial-dispose class is fine for THIS slice (the throw-carve-out
// restriction is on the consumer side).
BEZEL_TEST(pure_modifiers_2052, pure_constructor_signature_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _v: int; "
        "  public pure constructor(v: int) _v := v; end constructor; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15: a pure constructor also rejects `ref`/`out`/`owned`/`scoped`
// parameters — the parameter-modifier rule is member-wide.
BEZEL_TEST(pure_modifiers_2052, pure_constructor_owned_param_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure constructor(owned other: C) end constructor; "
        "  destructor end destructor; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "a `pure` member declares no"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "`owned`"));
}
