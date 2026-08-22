// Spec-derived sema tests for fix #3272 (v0.262, §22.12 rule 3/4):
// a `static readonly` field or auto-property whose declared type is a bare
// generic parameter of the enclosing TYPE (T, T?) is admitted at the
// declaration and classified per instantiation, after substitution:
//
//   - RULE 3 (immutable content — value types of sendable-value category,
//     string, enums): unrestricted. No diagnostic.
//   - RULE 4 (class type): declaration-site-class warning, silenced for
//     the warning-suppression set (FrozenArray<T>, CancellationTokenSource,
//     Locked<T>, RwLocked<T>; the form-(i) synchronized set is also
//     admitted: Mutex, Semaphore, Atomic<T>, AtomicInteger<T>,
//     ChannelSender<T>, SharedEventSource<T>).
//   - INTERFACE / FUNCTION substitution: rule 4 warning explicitly, no
//     suppression.
//   - OWNING-CLASSIFIED CHOICE substitution: rejection per the residual
//     paragraph (no rule enrols an owning-choice static readonly in
//     §22.13's program-exit disposal, so the value would be process-lived
//     with no disposal site).
//   - OWNING-INLINE-ARRAY substitution: default rule 4 warning.
//
// The diagnostic lands at the INSTANTIATION site and names the offending
// type argument (§3.6 rule 2's diagnostic pattern, v0.240).
//
// Spec citations: §22.12 rule 3/4 (v0.262, #3269 ruling, #3272 impl);
// §3.6 rule 2 (per-instantiation evaluation for generics).
// Companion: §9.7 records (and §9.7's static-field bullet) on the
// borrow-bearing / null-source arm; §35.4 choice classification on the
// owning-classified-choice arm.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
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
// §22.12 rule 3 (v0.262, #3272): IMMUTABLE CONTENT substitutions
// =========================================================================

// RULE 3 ACCEPT: `Box<int>` instantiation of `static readonly F: T` —
// `int` is a primitive (rule 3's "value types of sendable-value category"),
// unrestricted. The v0.262 ruling's headline ACCEPT case.
BEZEL_TEST(static_readonly_bare_t_3272,
           readonly_bare_t_with_int_accepted_rule_3_spec_22_12_issue_3272) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> "
        "  public static readonly F: T; "
        "  public static method Main(): int return 0; end Main; "
        "end Box; "
        "class Program "
        "  public static method Main(): int "
        "    var b := new Box<int>(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "§22.12 rule 4"));
}

// RULE 3 ACCEPT: `Box<string>` instantiation — `string` is in rule 3's
// category alongside primitives. Nullable form classifies as underlying
// type, so a `static readonly F: T?` with `T = string` is also rule 3.
BEZEL_TEST(static_readonly_bare_t_3272,
           readonly_bare_t_nullable_with_string_accepted_rule_3_spec_22_12_issue_3272) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> "
        "  public static readonly F: T?; "
        "  public static method Main(): int return 0; end Main; "
        "end Box; "
        "class Program "
        "  public static method Main(): int "
        "    var b := new Box<string>(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "§22.12 rule 4"));
}

// RULE 3 ACCEPT: enum substitution — enums are listed alongside primitives
// and string in rule 3's "immutable content" category.
BEZEL_TEST(static_readonly_bare_t_3272,
           readonly_bare_t_with_enum_accepted_rule_3_spec_22_12_issue_3272) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class Box<T> "
        "  public static readonly F: T; "
        "end Box; "
        "class Program "
        "  public static method Main(): int "
        "    var b := new Box<Color>(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "§22.12 rule 4"));
}

// =========================================================================
// §22.12 rule 4 (v0.262, #3272): CLASS-TYPED substitutions
// =========================================================================

// RULE 4 WARN (HEADLINE): `Box<Widget>` instantiation of `static readonly
// F: T` — Widget is a user-declared class, NOT in the warning-suppression
// set, so rule 4's declaration-site-class warning lands at the
// instantiation site (the `new Box<Widget>()` call) and names the offending
// type argument (`Widget`).
BEZEL_TEST(static_readonly_bare_t_3272,
           readonly_bare_t_with_class_warns_at_instantiation_rule_4_spec_22_12_issue_3272) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget "
        "  public method Render(): int return 42; end Render; "
        "end Widget; "
        "class Box<T> "
        "  public static readonly F: T; "
        "end Box; "
        "class Program "
        "  public static method Main(): int "
        "    var b := new Box<Widget>(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.12 rule 4"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "Widget"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§3.6 rule 2"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "#3272"));
}

// RULE 4 WARN (AUTO-PROPERTY): the same rule applies to static auto-
// properties — the spec text covers "field or auto-property" together,
// and the diagnostic distinguishes them by member-kind wording.
BEZEL_TEST(static_readonly_bare_t_3272,
           readonly_bare_t_autoproperty_with_class_warns_rule_4_spec_22_12_issue_3272) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget "
        "  public method Render(): int return 42; end Render; "
        "end Widget; "
        "class Box<T> "
        "  public static readonly property F: T; "
        "end Box; "
        "class Program "
        "  public static method Main(): int "
        "    var b := new Box<Widget>(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.12 rule 4"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "property"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "Widget"));
}

// RULE 4 ACCEPT: `Box<Mutex>` instantiation — Mutex is in the form-(i)
// synchronized set which is also admitted by rule 4's suppression set.
// The warning is silenced; no diagnostic at the instantiation site.
BEZEL_TEST(static_readonly_bare_t_3272,
           readonly_bare_t_with_mutex_accepted_synchronized_set_spec_22_12_issue_3272) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> "
        "  public static readonly F: T; "
        "end Box; "
        "class Program "
        "  public static method Main(): int "
        "    var b := new Box<Mutex>(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "§22.12 rule 4"));
}

// RULE 4 ACCEPT: `Box<FrozenArray<int>>` instantiation — FrozenArray<T>
// is in rule 4's broader warning-suppression set (the four members outside
// rule 2's form (i) per v0.262, #3272). The warning is silenced.
BEZEL_TEST(static_readonly_bare_t_3272,
           readonly_bare_t_with_frozen_array_accepted_suppression_set_spec_22_12_issue_3272) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> "
        "  public static readonly F: T; "
        "end Box; "
        "class Program "
        "  public static method Main(): int "
        "    var b := new Box<FrozenArray<int>>(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "§22.12 rule 4"));
}

// RULE 4 ACCEPT: `Box<Mutex>` with nullable form `T?` — the nullable
// form classifies as the underlying type per spec, and Mutex is in the
// suppression set, so still silenced.
BEZEL_TEST(static_readonly_bare_t_3272,
           readonly_bare_t_nullable_with_mutex_accepted_spec_22_12_issue_3272) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> "
        "  public static readonly F: T?; "
        "end Box; "
        "class Program "
        "  public static method Main(): int "
        "    var b := new Box<Mutex>(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMsg(r, "§22.12 rule 4"));
}

// =========================================================================
// §22.12 mutable arm (v0.230, #2331): declaration-site rejection
// =========================================================================

// #2331 / §22.12 rule 2 closing sentence (v0.230): a mutable static field
// whose declared type is a bare generic parameter of the enclosing TYPE
// (T, T?) is admitted by no form and is a COMPILE ERROR at the declaration
// site, whatever the parameter's constraints or eventual substitutions.
// This is the companion arm to the v0.262 readonly classification above;
// per the issue body, the two arms should be implemented together so the
// bare-parameter statics story lands whole.
BEZEL_TEST(static_readonly_bare_t_3272,
           mutable_bare_t_rejected_at_declaration_spec_22_12_issue_2331) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> "
        "  public static F: T; "
        "  public static method Main(): int return 0; end Main; "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.12 rule 2"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "mutable static field"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "T"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "#2331"));
}

// Same rule, nullable form. §22.12 rule 2's closing sentence explicitly
// names "or its nullable form (`T`, `T?`)" — the rejection covers both.
BEZEL_TEST(static_readonly_bare_t_3272,
           mutable_bare_t_nullable_rejected_at_declaration_spec_22_12_issue_2331) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> where T: class "
        "  public static shared: T?; "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.12 rule 2"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "#2331"));
}

// Same rule, auto-property form.
BEZEL_TEST(static_readonly_bare_t_3272,
           mutable_bare_t_autoproperty_rejected_at_declaration_spec_22_12_issue_2331) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> "
        "  public static property F: T; "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.12 rule 2"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "mutable static auto-property"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "#2331"));
}

// =========================================================================
// Multi-instantiation §3.6 rule 2 diagnostic shape (one diagnostic per
// (instantiation site, member, parameter))
// =========================================================================

// Two instantiations of the same `static readonly F: T` declaration with
// class-typed arguments. Each instantiation site that binds the class
// argument gets its own rule 4 warning (the parameter IS in the diagnostic
// key; §3.6 rule 2). The instantiation site itself names the offending
// type argument.
BEZEL_TEST(static_readonly_bare_t_3272,
           two_class_instantiations_each_warn_with_offending_arg_spec_22_12_issue_3272) {
    auto r = parseSemaAll(
        "module M; "
        "class WidgetA "
        "  public method Render(): int return 1; end Render; "
        "end WidgetA; "
        "class WidgetB "
        "  public method Render(): int return 2; end Render; "
        "end WidgetB; "
        "class Box<T> "
        "  public static readonly F: T; "
        "end Box; "
        "class Program "
        "  public static method Main(): int "
        "    var a := new Box<WidgetA>(); "
        "    var b := new Box<WidgetB>(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "WidgetA"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "WidgetB"));
    int warnings = countMsgs(r, "§22.12 rule 4", "#3272");
    BEZEL_EXPECT_EQ(warnings, 2);
}

// One instantiation with the rule-3 immutable content (no warning), one
// with the rule-4 class type (warning) — the parameter keying means the
// class substitution produces exactly one rule 4 warning and the immutable
// substitution produces none.
BEZEL_TEST(static_readonly_bare_t_3272,
           mixed_substitutions_per_instantiation_spec_22_12_issue_3272) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget "
        "  public method Render(): int return 1; end Render; "
        "end Widget; "
        "class Box<T> "
        "  public static readonly F: T; "
        "end Box; "
        "class Program "
        "  public static method Main(): int "
        "    var i := new Box<int>(); "       // rule 3 — clean
        "    var w := new Box<Widget>(); "    // rule 4 — warn
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    int warnings = countMsgs(r, "§22.12 rule 4", "#3272");
    BEZEL_EXPECT_EQ(warnings, 1);
}

// §22.12 rule 4 (v0.262, #3272) RESIDUAL PARAGRAPH, "owning-classified
// choice" bullet: "An owning-classified choice (§35.4 rule 1) is a compile
// error at the declaration site naming this rule, whether or not it also
// holds borrows: the previous bullet's null-source admission does not
// reach it, since an `owned` payload is process-lived with no disposal
// site even when every borrow-carrying component is `null`." The
// per-instantiation extension at v0.262 reads: "an owning-choice
// substitution is rejected on that paragraph's disposal-site ground."
//
// The current classifyStaticReadonlySubstitution predicate conservatively
// rejects EVERY choice substitution at the instantiation site (the §35.4
// ruling verdict on the substituted choice is not yet factored into a
// shared sema-readable helper, so this lane ships the conservative
// behaviour with the cited spec wording — a future narrowing lane can
// consult §35.4's verdict and admit value-like choices). This test pins
// the conservative behaviour so the narrowing has a clear upgrade path.
//
// The test would FAIL pre-fix (a `static readonly F: T;` choice
// substitution would silently compile, because the per-instantiation
// predicate did not exist); it PASSES post-fix because the conservative
// rejection branch in classifyStaticReadonlySubstitution fires at the
// `new Box<MyChoice>()` instantiation site.
BEZEL_TEST(static_readonly_bare_t_3272,
           choice_substitution_rejected_spec_22_12_issue_3272) {
    auto r = parseSemaAll(
        "module M; "
        "choice MyChoice IntVal(value: int), StrVal(value: string); end MyChoice; "
        "class Box<T> "
        "  public static readonly shared: T; "
        "end Box; "
        "class Program "
        "  public static method Main(): int "
        "    var b := new Box<MyChoice>(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    // Conservative rejection: the choice substitution is rejected at the
    // instantiation site per the §22.12 rule 4 residual paragraph's
    // owning-classified-choice bullet. The diagnostic cites §22.12 (the
    // parent rule) and #3272 (this lane's tracker).
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.12"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "#3272"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "MyChoice"));
    // The conservative predicate names the §35.4 narrowing path as the
    // future-upgrade hook for this rejection.
    BEZEL_EXPECT_TRUE(hasMsg(r, "§35.4"));
}
