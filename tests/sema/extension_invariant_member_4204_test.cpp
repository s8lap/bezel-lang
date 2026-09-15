// Spec-derived tests for fix #4204 — an `invariant` member on an
// `extension` block body compiled with ZERO diagnostics: the THIRD instance
// of the bug class fix #4115 closed for enum/choice bodies and fix #4186
// closed for record and static-class bodies (§22.16, v0.298).
//
// §22.16 ("Contract clauses"): "A method or constructor declaration may
// carry contract clauses ... and a class body may declare invariant
// members (`invariant(E);`)." An extension block is not a class body, and
// §22.16's own "Scope" subsection states directly (in its record/enum
// methods parenthetical): "the *invariant* member is class-only".
//
// Independent structural authority — Appendix A.4's `ExtensionMember`
// grammar production:
//   ExtensionMember ::= MethodDecl | ExpressionPropertyDecl
// `InvariantDecl` is not one of the two alternatives, so `invariant` is not
// even a grammatically valid extension member — yet it parses today because
// `Parser::parseExtensionDecl` (src/parser/Parser_Decls.cpp) calls the
// general `parseMember()` (the same shared production used for class/
// record/enum bodies) rather than a dedicated extension-only parse
// function. §26.3 ("Restrictions") independently confirms the same
// boundary in prose: "Only methods and computed read-only properties."
//
// NOTE: the Appendix A preamble's "Shared-production over-admissions"
// index table does not carry a row for extension bodies at all (nor,
// pre-#4186, for record or static-class bodies — see issue #4205, which
// reports the table as incomplete). Its silence is not evidence either way;
// the grammar production and §26.3/§22.16 prose above are the authority
// this fix relies on.
//
// `invariant` clauses are not modeled as a `Symbol` subclass at all —
// `SymbolCollection::collectMemberImpl` has no `ast::InvariantDecl` arm and
// returns null for it ("Unknown Member kind") — so `ExtensionSymbol::members`
// never carries one either; only the raw AST member list sees it. This fix
// extends the EXISTING §26.3 per-member restriction loop in
// `ExprTC::visitTopDecl`'s `ExtensionDecl` arm (src/sema/ExprTypecheck.cpp,
// ~line 11145) — which already walks `ext->members` and already rejects one
// other flat-out-forbidden AST member kind (`OperatorDecl`, §26.3 #936) with
// the identical dynamic_cast/report/continue shape — rather than adding a
// THIRD free-standing "walk members, find InvariantDecl, report" pass
// alongside `checkEnumChoiceInvariantMembers` (#4115) and
// `checkRecordAndStaticClassInvariantMembers` (#4186):
// `Resolver::forEachTypeAndNested` (src/sema/NestedTypeWalk.hpp) deliberately
// never visits an `ExtensionSymbol` (documented: "an `ExtensionSymbol` is
// visited only where a pass opts in"), so a new Resolver-side pass would have
// had to hand-roll its own `module()->children` walk — a second walking
// mechanism for the same decision, not a third spelling of one.
//
// MEASURED on r18/4186 base `33e1fc3` (pre-fix, #4186 already applied):
//   extension Widget4204 invariant(true); public method DoIt(): int => 1; end extension;
// compiled via `runAot` at exit 0, zero diagnostics (the issue's own repro,
// renamed to avoid symbol collisions with other test files in this binary).

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
// REJECT — §22.16 "Scope" ("the invariant member is class-only") / §26.3
// ("Only methods and computed read-only properties") / Appendix A.4
// `ExtensionMember` grammar.
// =====================================================================

// The issue's own repro, verbatim in shape (class name suffixed 4204 to
// avoid cross-file symbol collisions in the shared `sema` binary).
BEZEL_TEST(extension_invariant_4204, extension_invariant_rejected_spec_22_16) {
    auto r = runAot(
        "module M; "
        "class Widget4204 "
        "  public X: int; "
        "end Widget4204; "
        "extension Widget4204 "
        "  invariant(true); "
        "  public method DoIt(): int => 1; "
        "end extension; "
        "class C4204 public static method Main(): int "
        "  return 0; "
        "end Main; end C4204;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "an extension block may not declare an `invariant` member"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.16"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§26.3"));
}

// REJECT — the `static invariant` spelling (A.4 `InvariantDecl`'s optional
// `"static"?` qualifier) exercises a different `InvariantDecl::isStatic`
// value but the SAME AST node kind; this check does not read `isStatic` at
// all (the member-KIND rejection applies before the clause qualifier is
// even relevant), so it must reject this identically.
BEZEL_TEST(extension_invariant_4204,
           extension_static_invariant_rejected_spec_22_16) {
    auto r = runAot(
        "module M; "
        "class Widget4204s "
        "  public X: int; "
        "end Widget4204s; "
        "extension Widget4204s "
        "  static invariant(true); "
        "  public method DoIt(): int => 1; "
        "end extension; "
        "class C4204s public static method Main(): int "
        "  return 0; "
        "end Main; end C4204s;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "an extension block may not declare an `invariant` member"));
}

// REJECT — an extension on a PRIMITIVE type (§26.2 "Built-in primitive
// types" is extendable) carries no target `TypeSymbol` at all
// (`currentType_` stays null for a primitive extension, per the comment at
// ExprTypecheck.cpp's ExtensionDecl arm). The new check is purely
// AST-syntactic (reads `m.get()`'s dynamic type before any target-type
// resolution matters), so it must reject identically with no TypeSymbol in
// play.
BEZEL_TEST(extension_invariant_4204,
           extension_on_primitive_invariant_rejected_spec_22_16) {
    auto r = runAot(
        "module M; "
        "extension int "
        "  invariant(true); "
        "  public method Twice4204(): int => this * 2; "
        "end extension; "
        "class C4204p public static method Main(): int "
        "  return 0; "
        "end Main; end C4204p;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "an extension block may not declare an `invariant` member"));
}

// =====================================================================
// ACCEPT — legitimate extension bodies (methods, expression-bodied
// properties, both on a class target and a primitive target), with NO
// `invariant` member, must keep compiling (LANE_RULES §15 — a fix that adds
// a new rejection must not become a false reject of what the spec admits).
// =====================================================================

// ACCEPT — an ordinary extension on a class, method + expression-bodied
// property, no invariant member: unaffected by this fix.
BEZEL_TEST(extension_invariant_4204,
           extension_method_and_property_no_invariant_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class Widget4204a "
        "  public X: int; "
        "  public constructor(x: int) "
        "    X := x; "
        "  end constructor; "
        "end Widget4204a; "
        "extension Widget4204a "
        "  public method DoIt(): int => this.X + 1; "
        "  public property Doubled: int => this.X * 2; "
        "end extension; "
        "class C4204a public static method Main(): int "
        "  var w := new Widget4204a(20); "
        "  return w.DoIt() + w.Doubled; "
        "end Main; end C4204a;"),
        // DoIt() = 21, Doubled = 40 -> 61
        61);
}

// ACCEPT — an extension on a primitive type, no invariant member: unaffected
// by this fix (pairs with the primitive-target reject row above).
BEZEL_TEST(extension_invariant_4204,
           extension_on_primitive_no_invariant_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "extension int "
        "  public method Twice4204b(): int => this * 2; "
        "end extension; "
        "class C4204b public static method Main(): int "
        "  var x: int := 21; "
        "  return x.Twice4204b(); "
        "end Main; end C4204b;"),
        42);
}

// ACCEPT — a class's own `invariant` member (§22.16's ordinary case) is
// entirely unaffected by this extension-only check: this fix's new code
// lives inside the ExtensionDecl arm of `ExprTC::visitTopDecl` only.
BEZEL_TEST(extension_invariant_4204,
           class_invariant_accepted_control_spec_22_16) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class Account4204 "
        "  private _balance: decimal := 0m; "
        "  private _limit: decimal := 0m; "
        "  invariant(_balance + _limit >= 0m); "
        "  public constructor(limit: decimal) "
        "    _limit := limit; "
        "  end constructor; "
        "end Account4204; "
        "class C4204c public static method Main(): int "
        "  var a := new Account4204(5m); "
        "  return 42; "
        "end Main; end C4204c;"),
        42);
}
