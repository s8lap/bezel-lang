// Spec-derived tests for fix #4115 — an `invariant` member on an `enum` or
// `choice` body compiled with ZERO diagnostics.
//
// §22.16 ("Contract clauses", v0.298): "A method or constructor declaration
// may carry contract clauses ... and a class body may declare invariant
// members (`invariant(E);`)." An enum or a choice is not a class, and the
// spec's Appendix A preamble's "Shared-production over-admissions" index —
// the UNNUMBERED table introduced "Shared-production over-admissions
// (index, not a rule)" immediately before A.1, NOT a numbered subsection —
// confirms this is deliberate rejected policy, not an oversight: its
// `Member` in an enum-or-choice body row lists `invariant` among the forms
// the shared `Member` grammar production admits syntactically but the
// language rejects semantically, citing "`invariant` by §22.16". (The
// filing issue's own TITLE cited this table as "Appendix A.9" — VERIFIED
// against docs/Bezel_Language_Spec.md that no such subsection exists;
// Appendix A's numbered subsections run A.1 through A.8 only. This file
// cites the table by its real location: the unnumbered preamble table.)
//
// `invariant` clauses are not modeled as a `Symbol` subclass in
// `TypeSymbol::members` at all — `SymbolCollection` never creates one for
// `ast::InvariantDecl` — so neither `Resolver::checkEnumRestrictions` nor
// `Resolver::checkChoiceRestrictions` (both walking `t->members`) could ever
// see it. This fix adds `Resolver::checkEnumChoiceInvariantMembers`
// (TypeResolution_Checks.cpp), which reads the raw AST member list
// (`t->decl`'s `ast::TypeDecl::members`) directly instead.
//
// MEASURED on origin/main-derived R17 cycle-3 base `3494cd1` (pre-fix):
//   choice Shape Circle(radius: double), Square(side: double);
//     invariant(true);
//   end Shape;
// and
//   enum Color Red, Green, Blue; invariant(true); end Color;
// both compiled with `./build/bezelc -c` at exit 0, zero diagnostics.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// =====================================================================
// REJECT — §22.16 / Appendix A preamble's `Member` in an enum-or-choice
// body row.
// =====================================================================

BEZEL_TEST(enum_choice_invariant_4115, enum_invariant_rejected_spec_22_16) {
    auto r = runAot(
        "module M; "
        "enum Color4115 "
        "  Red, Green, Blue; "
        "  invariant(true); "
        "end Color4115; "
        "class C4115 public static method Main(): int "
        "  return 0; "
        "end Main; end C4115;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "enum `Color4115` may not declare an `invariant` member"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.16"));
}

BEZEL_TEST(enum_choice_invariant_4115, choice_invariant_rejected_spec_22_16) {
    auto r = runAot(
        "module M; "
        "choice Shape4115 "
        "  Circle(radius: double), Square(side: double); "
        "  invariant(true); "
        "end Shape4115; "
        "class C4115b public static method Main(): int "
        "  return 0; "
        "end Main; end C4115b;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "choice `Shape4115` may not declare an `invariant` member"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.16"));
}

// REJECT — the `static invariant` spelling. v0.338 (#5939): an invariant
// member takes no `static` qualifier, so the qualifier is rejected at the
// parser (its needle is pinned by tests/sema/static_invariant_*) while the
// body-kind rejection of this file's own kind is what this row is about.
BEZEL_TEST(enum_choice_invariant_4115,
           enum_static_invariant_rejected_spec_22_16) {
    auto r = runAot(
        "module M; "
        "enum Color4115s "
        "  Red, Green, Blue; "
        "  static invariant(true); "
        "end Color4115s; "
        "class C4115s public static method Main(): int "
        "  return 0; "
        "end Main; end C4115s;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "an invariant member takes no `static` qualifier"));
}

// =====================================================================
// ACCEPT — legitimate uses of `invariant` (class bodies), and legitimate
// enum/choice member forms without `invariant`, must keep compiling
// (LANE_RULES §15 — a fix that adds a new rejection must not become a false
// reject of what the spec actually admits).
// =====================================================================

// ACCEPT — the spec's own §22.16 worked example (`Account`), verbatim in
// shape: a class invariant must be entirely unaffected by an enum/choice-
// only check.
BEZEL_TEST(enum_choice_invariant_4115,
           class_invariant_accepted_control_spec_22_16) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class Account4115 "
        "  private _balance: decimal := 0m; "
        "  private _limit: decimal := 0m; "
        "  invariant(_balance + _limit >= 0m); "
        "  public constructor(limit: decimal) "
        "    _limit := limit; "
        "  end constructor; "
        "end Account4115; "
        "class C4115c public static method Main(): int "
        "  var a := new Account4115(5m); "
        "  return 42; "
        "end Main; end C4115c;"),
        42);
}

// ACCEPT — an enum with ordinary §10.4-admitted members (instance method,
// expression-bodied read-only property, static owned readonly field) and NO
// invariant member must keep compiling.
BEZEL_TEST(enum_choice_invariant_4115,
           enum_ordinary_members_accepted_control_spec_10_4) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "enum Priority4115 "
        "  Low, Medium, High; "
        "  public method IsUrgent(): bool => this = High; "
        "  public property Name: string => (case this of "
        "    Low => \"Low\", Medium => \"Medium\", High => \"High\"); "
        "end Priority4115; "
        "class C4115d public static method Main(): int "
        "  var p := Priority4115.High; "
        "  return (if p.IsUrgent() and p.Name = \"High\" then 42 else 1); "
        "end Main; end C4115d;"),
        42);
}

// ACCEPT — a choice with ordinary §35.2-admitted members AND a real payload,
// no invariant member.
BEZEL_TEST(enum_choice_invariant_4115,
           choice_ordinary_members_with_payload_accepted_control_spec_35_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "choice Shape4115b "
        "  Circle(radius: double), Square(side: double); "
        "  public method DescribeIt(): string => "
        "    (case this of Circle(_) => \"circle\", Square(_) => \"square\"); "
        "  public static readonly Zero4115b: double := 0.0; "
        "  public const Label4115b: string := \"shape\"; "
        "end Shape4115b; "
        "class C4115e public static method Main(): int "
        "  var s := Shape4115b.Circle(2.0); "
        "  return (if s.DescribeIt() = \"circle\" and "
        "             Shape4115b.Zero4115b = 0.0 and "
        "             Shape4115b.Label4115b = \"shape\" "
        "          then 42 else 1); "
        "end Main; end C4115e;"),
        42);
}

// ACCEPT — a generic choice, no invariant member: this check reads only
// `t->kind` and the raw member list, so it cannot be sensitive to generic
// parameters.
BEZEL_TEST(enum_choice_invariant_4115, generic_choice_accepted_control_spec_35_11) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "choice Box4115<T> Empty4115, Full4115(value: T); end Box4115; "
        "class C4115f public static method Main(): int "
        "  var b := Box4115<int>.Full4115(6); "
        "  return (case b of Empty4115 => 1, Full4115(v) => v * 7); "
        "end Main; end C4115f;"),
        42);
}

// A record's own `invariant` member was left uncovered by THIS fix (#4115
// scoped to enum/choice only) and was, until fix #4186 landed, admitted with
// zero diagnostics — see that fix's own
// tests/sema/record_static_class_invariant_member_4186_test.cpp for the
// reject-side coverage (records and static classes both) now that the gap
// is closed. Not re-asserted here: this file's scope is enum/choice, and a
// stale "still admitted" control here would silently rot the day #4186's
// fix changes this program's outcome, exactly the failure mode #4186's test
// file guards against directly.
