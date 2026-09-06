// Spec-derived sema tests for fix #3290 (v0.265, §22.12 rule 4 residual):
// the owning-choice rejection and the default declaration-site warning
// (function-typed static, owning-inline-array-typed static, records with
// function-typed fields, etc.), plus the §9.7 null-source value-site check
// for borrow-bearing records / holds-borrows choices / plain inline arrays
// carrying borrows.
//
// The v0.265 ruling (#3273) widens §22.12 rule 4's declaration-site
// default warning to every declared type that rules 3 and 4 do not name,
// with three kinds excepted in the residual paragraph:
//
//   1. Owning-classified choice → COMPILE ERROR at the declaration site
//      (the disposal-site ground).
//   2. Borrow-bearing record / non-owning choice that holds borrows /
//      plain inline array carrying borrows → §9.7 null-source case
//      (value-site check rejects non-null reference-typed components).
//   3. Function type, owning-inline-array type → default warning.
//
// Spec citations: §22.12 rule 4 residual (v0.265, #3273 ruling); §9.7
// static-field bullet (the null-source form, v0.266, #3291 ruling); §35.4
// (choice classification); §22.13 (program-exit disposal). Companion:
// #3272 (the rule-3/4 bare-parameter arm) and #3271 (the §14.6 expand).

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

// fix #3985: delegates to the shared gated helper (sema_support.hpp)
// instead of duplicating the parse-error-gate logic locally.
bezel::test::SemaResult parseSemaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
}

// True iff some reported diagnostic's message contains `needle`.
bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// Count of diagnostics whose message contains BOTH needles (substring AND).
int countMsgs(const bezel::test::SemaResult& r,
              const std::string& a, const std::string& b) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(a) != std::string::npos &&
            d.message.find(b) != std::string::npos) ++n;
    return n;
}

} // namespace

// =========================================================================
// §22.12 rule 4 residual (v0.265, #3290): OWNING-CLASSIFIED CHOICE rejection
// =========================================================================

// REJECT (HEADLINE): a `static readonly` field of an owning-classified
// choice is rejected at the declaration site, naming §22.12 rule 4's
// residual. The diagnostic cites the disposal-site ground — no rule
// enrols an owning-choice static in §22.13's program-exit disposal.
BEZEL_TEST(static_readonly_residual_3290,
           owning_choice_rejected_at_declaration_spec_22_12_issue_3290) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget "
        "  public method Render(): int return 42; end Render; "
        "end Widget; "
        "choice MyChoice Fetched(owned value: Widget), Plain(value: int); "
        "  end MyChoice; "
        "class Holder "
        "  public static readonly F: MyChoice; "
        "end Holder;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "static readonly"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "owning-classified choice"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "MyChoice"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.12"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "#3290"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§35.4"));
}

// REJECT (owning choice + holds borrows): the residual paragraph's bullet
// explicitly states the rejection takes precedence over the null-source
// admission even when the choice ALSO holds borrows with every borrow-
// carrying component null. This is the overlap the v0.265 composition gate
// found and closed; a test that does not cover it leaves the hole open.
BEZEL_TEST(static_readonly_residual_3290,
           owning_choice_holds_borrows_still_rejected_spec_22_12_issue_3290) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget "
        "  public method Render(): int return 42; end Render; "
        "end Widget; "
        "choice MyChoice "
        "  Fetched(owned value: Widget, src: Object?), "
        "  Plain(value: int); "
        "  end MyChoice; "
        "class Holder "
        "  public static readonly F: MyChoice; "
        "end Holder;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "owning-classified choice"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.12"));
}

// REJECT (auto-property form): same rule, auto-property shape. The
// residual paragraph covers "field or auto-property" together.
BEZEL_TEST(static_readonly_residual_3290,
           owning_choice_autoproperty_rejected_spec_22_12_issue_3290) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget "
        "  public method Render(): int return 42; end Render; "
        "end Widget; "
        "choice MyChoice Fetched(owned value: Widget), Plain(value: int); "
        "  end MyChoice; "
        "class Holder "
        "  public static readonly property F: MyChoice; "
        "end Holder;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "owning-classified choice"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "auto-property") ||
                     hasMsg(r, "property"));
}

// ACCEPT (value-like choice): a value-like choice (no owned payloads,
// no borrow-bearing payloads) is not owning-classified, so it does not
// fall into the rejection bullet. It IS outside rule 3's immutable
// content (rule 3 is value types of sendable-value category / string /
// enums), so the default warning fires. Verifies the rejection is
// scoped to owning-classified choices, not all choices.
BEZEL_TEST(static_readonly_residual_3290,
           value_like_choice_static_readonly_warns_spec_22_12_issue_3290) {
    auto r = parseSemaAll(
        "module M; "
        "choice ColorVal Red(r: int), Green(g: int), Blue(b: int); "
        "  end ColorVal; "
        "class Holder "
        "  public static readonly F: ColorVal; "
        "end Holder;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "static readonly"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.12 rule 4"));
    BEZEL_EXPECT_FALSE(hasMsg(r, "owning-classified"));
}

// =========================================================================
// §22.12 rule 4 residual (v0.265, #3290): FUNCTION-TYPED warning
// =========================================================================

// WARN (function type): the residual paragraph names function type as a
// kind that takes the default warning explicitly — captured variables of
// a function value are a heap-allocated mutable cell reachable from every
// domain through the field (§27.5).
BEZEL_TEST(static_readonly_residual_3290,
           function_typed_static_readonly_warns_spec_22_12_issue_3290) {
    auto r = parseSemaAll(
        "module M; "
        "class Holder "
        "  public static readonly F: (int) -> int; "
        "end Holder;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "static readonly"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "function"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.12 rule 4"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "#3290"));
}

// =========================================================================
// §22.12 rule 4 residual (v0.265, #3290): OWNING-INLINE-ARRAY warning
// =========================================================================

// WARN (owning-inline-array): the residual paragraph names
// owning-inline-array-typed static readonly fields explicitly — they take
// the default warning and ARE disposed at exit (§22.13's owning-inline-
// array clause is not conditioned on mutability).
BEZEL_TEST(static_readonly_residual_3290,
           owning_inline_array_typed_static_readonly_warns_spec_22_12_issue_3290) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget "
        "  public method Render(): int return 42; end Render; "
        "end Widget; "
        "class Holder "
        "  public static readonly F: [3 owned]Widget?; "
        "end Holder;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "static readonly"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "owning-inline-array"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.12"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.13"));
}

// =========================================================================
// §22.12 rule 4 residual (v0.265, #3290): DEFAULT warning for other types
// =========================================================================

// DEFER (record with function-typed field): a record with a function-typed
// field is non-borrow-bearing (function types are runtime-managed) but §9.7's
// stricter quantifier still classifies it as a reference-typed field — so the
// record IS borrow-bearing for §22.12's defer purposes. The residual paragraph
// defers it to §9.7's null-source check (no declaration-site diagnostic) and
// the value-site pass rejects non-null initializers.
BEZEL_TEST(static_readonly_residual_3290,
           record_with_function_field_defers_to_97_spec_22_12_issue_3290) {
    auto r = parseSemaAll(
        "module M; "
        "record Holder(handler: (int) -> int); "
        "class Box "
        "  public static readonly F: Holder; "
        "end Box;");
    // Declaration-site: NO rule-4 warning. The residual paragraph defers
    // borrow-bearing records (and now records with only reference-typed
    // fields like function / Object? / string) to §9.7 with NO
    // declaration-site diagnostic.
    BEZEL_EXPECT_FALSE(hasMsg(r, "static readonly"));
    BEZEL_EXPECT_FALSE(hasMsg(r, "§22.12 rule 4"));
    BEZEL_EXPECT_FALSE(hasMsg(r, "Holder"));
}

// REJECT (record with function-typed field, non-null initializer): the
// §9.7 null-source check rejects a non-null reference-typed field at
// the value site. A function-typed field IS reference-typed per §9.7's
// stricter quantifier (the v0.266 / #3291 ruling extends "every
// reference-typed field" beyond borrow-carrying to runtime-managed
// reference types), so `new Holder(someHandler)` is rejected because
// `handler` is reference-typed and the initializer is not `null`.
BEZEL_TEST(static_readonly_residual_3290,
           record_with_non_null_function_field_rejected_spec_22_12_issue_3290) {
    // fix #3985: `method SomeHandler(...) ... end SomeHandler;` was a bare
    // top-level function declaration — Bezel's `TopDecl` grammar (Appendix
    // A) admits only ClassDecl/InterfaceDecl/RecordDecl/EnumDecl at module
    // scope; there is no free-function form, so a method must live inside a
    // type. This never parsed, so the row was reaching sema on
    // parseSemaAll's ungated, error-recovery-truncated AST. VERIFIED
    // against the real `bezelc -c`: moving `SomeHandler` into a static
    // class and referencing it as `Utils.SomeHandler` parses cleanly and
    // reproduces the intended §9.7 null-source rejection on `Box.F`.
    auto r = parseSemaAll(
        "module M; "
        "class Widget "
        "  public method Render(): int return 42; end Render; "
        "end Widget; "
        "static class Utils "
        "  public static method SomeHandler(i: int): int return i + 1; end SomeHandler; "
        "end Utils; "
        "record Holder(handler: (int) -> int); "
        "class Box "
        "  public static readonly F: Holder := new Holder(Utils.SomeHandler); "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "handler"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "null-source"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§9.7"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "#3290"));
    // No rule-4 declaration-site warning (Finding 2's defer fix).
    BEZEL_EXPECT_FALSE(hasMsg(r, "confine its use to one domain"));
}

// WARN (value-like record): a record whose every field is a value type
// (int, bool, …) is not borrow-bearing and not in rule 3's immutable
// content, so the default rule 4 warning fires.
BEZEL_TEST(static_readonly_residual_3290,
           value_like_record_warns_spec_22_12_issue_3290) {
    auto r = parseSemaAll(
        "module M; "
        "record Pair(X: int, Y: int); "
        "class Box "
        "  public static readonly F: Pair; "
        "end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "static readonly"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.12 rule 4"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "Pair"));
}

// =========================================================================
// §9.7 static-field bullet (v0.266, #3291 ruling) via §22.12 rule 4
// residual (v0.265, #3290): NULL-SOURCE admission for borrow-bearing
// records / holds-borrows choices / plain arrays carrying borrows.
// =========================================================================

// ACCEPT (CancellationToken sentinel, user-declared form): a user
// record with no reference-typed fields compiles when declared as a
// `static readonly property` sentinel — the rule-4 residual default
// warning fires (CancellationToken is outside rule 3's immutable
// content: it is a record, not a primitive / string / enum), but no
// §9.7 null-source error fires because the record has no reference-
// typed fields to violate the null-source form.
//
// NB: this test declares a USER `record CancellationToken`, NOT the
// compiler-known builtin. The builtin CancellationToken
// (`isValueBuiltin=true`, set in `SemaContext.hpp:934`) is a separate
// path — its sentinel `None` is admitted by the `isValueBuiltin` arm
// of the borrow-bearing predicate, which the test infra does not
// expose. The acceptance here is therefore the simpler shape: a record
// whose every field is a value-typed (non-reference) auto-property,
// which the §9.7 null-source check vacuously admits.
BEZEL_TEST(static_readonly_residual_3290,
           cancellation_token_none_compiles_spec_22_12_issue_3290) {
    auto r = parseSemaAll(
        "module M; "
        "public record CancellationToken "
        "  public static readonly property None: CancellationToken; "
        "  public readonly property IsCancellationRequested: bool; "
        "  public method ThrowIfCancellationRequested(); "
        "end CancellationToken;");
    // The sentinel `None` compiles — the record has no reference-typed
    // fields, so the §9.7 null-source check vacuously admits the
    // default-initialised value. No §9.7 error fires.
    BEZEL_EXPECT_FALSE(hasMsg(r, "null-source"));
    BEZEL_EXPECT_FALSE(hasMsg(r, "owning-classified"));
}

// ACCEPT (parameter default `token := CancellationToken.None`): the
// every-default form of the standard cancellation parameter must compile.
// Same predicate as the sentinel above — the user record has no
// reference-typed fields, so the §9.7 null-source check vacuously
// admits both the sentinel declaration and the parameter default.
BEZEL_TEST(static_readonly_residual_3290,
           token_default_cancellation_none_compiles_spec_22_12_issue_3290) {
    auto r = parseSemaAll(
        "module M; "
        "public record CancellationToken "
        "  public static readonly property None: CancellationToken; "
        "end CancellationToken; "
        "class Program "
        "  public method Do(token: CancellationToken := CancellationToken.None): int return 0; end Do; "
        "end Program;");
    BEZEL_EXPECT_FALSE(hasMsg(r, "null-source"));
}

// REJECT (record with non-null reference-typed field, v0.266 / #3291
// stricter quantifier): `record Tok(Src: Object?, Name: string)`
// initialised `new Tok(null, "missing")` is REJECTED at the value site
// because `Name` is reference-typed (`string`) and "missing" is not null
// — even though `Name` is NOT borrow-carrying (a `string` is runtime-
// managed). §9.7's stricter quantifier (every reference-typed field, not
// just every borrow-carrying one) is the rule. The §9.7 diagnostic names
// the offending FIELD ("Name"), not the record itself ("Tok"); the
// record's name is not asserted here on that account. NO declaration-
// site warning fires (the residual paragraph defers records whose only
// reference-typed fields are runtime-managed to §9.7 with no rule-4
// diagnostic).
BEZEL_TEST(static_readonly_residual_3290,
           record_with_non_null_string_field_rejected_spec_22_12_issue_3290) {
    auto r = parseSemaAll(
        "module M; "
        "record Tok(Src: Object?, Name: string); "
        "class Holder "
        "  public static readonly F: Tok := new Tok(null, \"missing\"); "
        "end Holder;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "Name"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "null-source"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§9.7"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "#3290"));
    // No rule-4 declaration-site warning (Finding 2's defer fix): the
    // record's only reference-typed fields are runtime-managed, so the
    // residual paragraph defers to §9.7 with NO declaration-site
    // diagnostic. Without this assertion, an earlier-broader
    // implementation would silently pass by emitting both (3d) warning
    // AND the §9.7 value-site error — the bug Finding 2 closed. Match
    // on the (3d) warning's unique "confine its use to one domain"
    // wording to avoid the §9.7 diagnostic's §-citation tail (which
    // includes "§22.12 rule 4 residual" as a reference).
    BEZEL_EXPECT_FALSE(hasMsg(r, "confine its use to one domain"));
}

// ACCEPT (record with all-null reference-typed fields): `new Tok(null,
// null)` is the §9.7 null-source form for `Tok(Src: Object?, Name:
// string)` (both `Src` and `Name` are null). Allowed at the value site.
BEZEL_TEST(static_readonly_residual_3290,
           record_with_all_null_reference_fields_accepted_spec_22_12_issue_3290) {
    auto r = parseSemaAll(
        "module M; "
        "record Tok(Src: Object?, Name: string); "
        "class Holder "
        "  public static readonly F: Tok := new Tok(null, null); "
        "end Holder;");
    // The declaration-site visit may still fire the default rule 4
    // warning (Tok is outside rule 3's immutable content — it's a
    // record), but that is a WARNING not an error. Acceptance here is
    // "no §9.7 error".
    BEZEL_EXPECT_FALSE(hasMsg(r, "null-source"));
}

// REJECT (record with non-null borrow-carrying field): the headline
// §9.7 case — `record Tok(Src: Object?)` initialised `new Tok(someObj)`
// is REJECTED because `Src` is a non-null reference-typed field.
BEZEL_TEST(static_readonly_residual_3290,
           record_with_non_null_borrow_field_rejected_spec_22_12_issue_3290) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget "
        "  public method Render(): int return 42; end Render; "
        "end Widget; "
        "record Tok(Src: Object?); "
        "class Holder "
        "  public static readonly F: Tok := new Tok(new Widget()); "
        "end Holder;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "Src"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "null-source"));
}

// =========================================================================
// AXIS GRID — orthogonal axes enumerated per [[lane-quality-selfpass]]
// (owning vs borrowing; function vs owning-inline-array vs all other;
// field vs auto-property; static vs instance).
// =========================================================================

// ACCEPT (instance readonly field — outside §22.12 entirely): an instance
// `readonly` field is not subject to §22.12's rules (it's per-instance,
// not process-global), so the residual check is silent for it.
BEZEL_TEST(static_readonly_residual_3290,
           instance_readonly_owning_choice_field_silent_spec_22_12_issue_3290) {
    // fix #3985: `public constructor();` has no body, but Appendix A's
    // ConstructorDecl admits no bare-`;` alternative (unlike an interface
    // method) — every instance constructor needs `CtorChain? Statement*
    // "end" "constructor"? ";"`. This never parsed, so the row was
    // reaching sema on parseSemaAll's ungated, error-recovery-truncated
    // AST. Since the non-nullable field `F` also needs definite assignment
    // (§7.9), the corrected constructor body assigns it. VERIFIED against
    // the real `bezelc -c`: the corrected source parses and typechecks
    // with zero diagnostics.
    auto r = parseSemaAll(
        "module M; "
        "class Widget "
        "  public method Render(): int return 42; end Render; "
        "end Widget; "
        "choice MyChoice Fetched(owned value: Widget), Plain(value: int); "
        "  end MyChoice; "
        "class Holder "
        "  public readonly F: MyChoice; "
        "  public constructor() F := MyChoice.Plain(0); end constructor; "
        "end Holder; "
        "class Program "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    // Instance-level readonly: §22.12's static-and-domains check does
    // not visit instance fields, so the rejection does not fire.
    BEZEL_EXPECT_FALSE(hasMsg(r, "owning-classified"));
}

// ACCEPT (function-typed static readonly in a record — outside the
// §22.12 static-and-domains surface entirely): §22.12 governs class and
// enum static members, not records.
BEZEL_TEST(static_readonly_residual_3290,
           record_static_function_field_silent_spec_22_12_issue_3290) {
    auto r = parseSemaAll(
        "module M; "
        "record Handler "
        "  public static readonly F: (int) -> int; "
        "end Handler;");
    // Records do not get §22.12 static-and-domains checks.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
