// Spec-derived tests for fix #4213 — the six remaining `Member` alternatives
// that Appendix A.4's `ExtensionMember` grammar does not admit
// (`ExtensionMember ::= MethodDecl | ExpressionPropertyDecl`) but that
// compiled inside an `extension` body with no §26.3 rejection, found while
// fixing #4204 (which closed the `InvariantDecl` instance of the same bug
// class for the `ExprTC::visitTopDecl` `ExtensionDecl` arm's §26.3 per-member
// restriction loop, `src/sema/ExprTypecheck.cpp`).
//
// §26.3 ("Restrictions") states the closed list in prose:
//   "Only methods and computed read-only properties."
//   "No fields, auto-properties, constructors, destructors, or `owned`
//   fields."
//   "No indexers."
// Appendix A.4's grammar states the same boundary structurally:
//   Member          ::= FieldDecl | MethodDecl | PropertyDecl | IndexerDecl
//                     | ConstructorDecl | DestructorDecl
//                     | OperatorDecl | NestedTypeDecl | ConstDecl
//                     | PolymorphicMember | InvariantDecl
//   ExtensionMember ::= MethodDecl | ExpressionPropertyDecl
//
// MEASURED against r18-integration base `53f8aa0` (this run, #4204 fix
// applied, pre-#4213): of the issue's six named kinds —
//   - FieldDecl, DestructorDecl, ConstDecl, NestedTypeDecl: each compiled at
//     exit 0 with ZERO diagnostics (silently admitted, matching the issue's
//     own `ConstDecl` repro).
//   - ConstructorDecl: did NOT silently compile — it reached a SIGSEGV in
//     codegen (`bezel::codegen::Emitter::emitImplicitBase`,
//     src/codegen/LlvmBackend_Setup.cpp:13272, called from
//     `emitConstructorBody`), because a constructor inside an extension has
//     no real `TypeSymbol`-owned base-class chain to walk. Confirmed with
//     `gdb --batch -ex run -ex bt`. This is a WORSE pre-existing defect than
//     "silently admitted": a rejection at sema (this fix) runs before
//     codegen and Driver::compileObject/compileFile never reaches the crash
//     site once sema reports an error.
//   - PolymorphicMember: REFUTED as an instance of "admitted with zero
//     diagnostics" — it has no distinct AST `Member` subclass at all (A.4's
//     `PolymorphicMember` clause mutates the enclosing `TypeDecl`'s
//     `serializedClass` field directly, `Parser::parsePolymorphicMember`,
//     src/parser/Parser_Decls.cpp) and that function is called only from
//     `parseTypeDeclBody`'s class/record member loop — never from the plain
//     `parseMember()` that `Parser::parseExtensionDecl`'s member loop calls.
//     So `polymorphic(...)` inside an extension body already fails to PARSE
//     (measured: cascading "expected member visibility ... or member-kind
//     keyword" errors, one per stray token) before any `ext->members` entry
//     for it could exist. Unaffected by this fix; its poor cascading-error
//     diagnostic quality is filed separately (issue 4222).
//
// A seventh kind not named by the issue, but excluded by the same
// authority (§26.3's explicit "No indexers." bullet, and the
// `ExtensionMember`-grammar's `ExpressionPropertyDecl` alternative, whose
// own A.4 production takes a plain `Identifier` name and never the
// indexer's `"[" ParamList "]"` form) was ALSO found admitted, though not
// silently: an indexer parses as a `PropertyDecl` with `isIndexer = true`
// (Parser_Decls.cpp's `property` arm — A.4 has no distinct `IndexerDecl`
// struct), so an expression-bodied indexer sailed past the existing
// property-body-kind check and reached `visitMember`, which failed
// downstream with a confusing "unresolved identifier" diagnostic instead of
// naming the real §26.3 rule.
//
// THE FIX: rather than six more near-duplicate `dynamic_cast`/report/
// `continue` arms (the shape fix #4204 and the OperatorDecl check already
// use), the per-member loop gets ONE default-deny catch-all: anything that
// reaches the end of the loop and is not a `MethodDecl` and not a (by that
// point already-guaranteed non-indexer, expression-bodied) `PropertyDecl`
// is inadmissible, independent of which of A.4's eleven `Member`
// alternatives it is — including any future twelfth one, which a per-kind
// allowlist would silently admit exactly as it did here for six releases.
// A dedicated `isIndexer` check was added ahead of the existing body-kind
// check (PropertyDecl IS one of the two admitted AST wrapper kinds, so the
// default-deny arm cannot see it) rather than folding into the catch-all.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// =====================================================================
// REJECT — the six confirmed-admitted `Member` alternatives (five silent,
// one a crash) plus the bonus `IndexerDecl` (PropertyDecl+isIndexer) case.
// Each asserts a REAL source location (never 0:0) and message text naming
// both the forbidden kind and the §26.3 authority, never a bare
// `hasErrors()`/`EXPECT_FALSE(r.ran)`.
// =====================================================================

// REJECT — FieldDecl. §26.3: "No fields, auto-properties, constructors,
// destructors, or `owned` fields." Issue-measured: exit 0, zero diagnostics
// pre-fix.
BEZEL_TEST(extension_member_kind_4213, extension_field_rejected_spec_26_3) {
    auto r = runAot(
        "module M; "
        "class WidgetF4213 "
        "  public X: int; "
        "end WidgetF4213; "
        "extension WidgetF4213 "
        "  public Y: int; "
        "  public method DoIt(): int => this.X; "
        "end extension; "
        "class CF4213 public static method Main(): int "
        "  return 0; "
        "end Main; end CF4213;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "an extension block may not declare a `field` member"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§26.3"));
}

// REJECT — ConstructorDecl. Pre-fix this reached a SIGSEGV in
// `Emitter::emitImplicitBase` (codegen), not a silent accept; this fix's
// sema-level rejection runs before codegen and prevents the crash entirely.
BEZEL_TEST(extension_member_kind_4213,
           extension_constructor_rejected_spec_26_3) {
    auto r = runAot(
        "module M; "
        "class WidgetCtor4213 "
        "  public X: int; "
        "end WidgetCtor4213; "
        "extension WidgetCtor4213 "
        "  public constructor() "
        "  end constructor; "
        "  public method DoIt(): int => this.X; "
        "end extension; "
        "class CC4213 public static method Main(): int "
        "  return 0; "
        "end Main; end CC4213;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "an extension block may not declare a `constructor` member"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§26.3"));
}

// REJECT — DestructorDecl. §26.3: "No fields, auto-properties,
// constructors, destructors, or `owned` fields." Issue-measured: exit 0,
// zero diagnostics pre-fix.
BEZEL_TEST(extension_member_kind_4213,
           extension_destructor_rejected_spec_26_3) {
    auto r = runAot(
        "module M; "
        "class WidgetDtor4213 "
        "  public X: int; "
        "end WidgetDtor4213; "
        "extension WidgetDtor4213 "
        "  destructor "
        "  end destructor; "
        "  public method DoIt(): int => this.X; "
        "end extension; "
        "class CD4213 public static method Main(): int "
        "  return 0; "
        "end Main; end CD4213;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "an extension block may not declare a `destructor` member"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§26.3"));
}

// REJECT — ConstDecl. The issue's own measured repro (against a #4204-only
// tree): exit 0, zero diagnostics.
BEZEL_TEST(extension_member_kind_4213, extension_const_rejected_spec_26_3) {
    auto r = runAot(
        "module M; "
        "class WidgetC4213 "
        "  public X: int; "
        "end WidgetC4213; "
        "extension WidgetC4213 "
        "  public static const K4213: int := 42; "
        "  public method DoIt(): int => this.X; "
        "end extension; "
        "class CK4213 public static method Main(): int "
        "  return 0; "
        "end Main; end CK4213;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "an extension block may not declare a `const` member"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§26.3"));
}

// REJECT — NestedTypeDecl (AST wrapper `ast::NestedTypeMember`). §26.3's
// first bullet ("Only methods and computed read-only properties.") is
// exhaustive; a nested type is neither. Issue-measured: exit 0, zero
// diagnostics pre-fix.
BEZEL_TEST(extension_member_kind_4213,
           extension_nested_type_rejected_spec_26_3) {
    auto r = runAot(
        "module M; "
        "class WidgetN4213 "
        "  public X: int; "
        "end WidgetN4213; "
        "extension WidgetN4213 "
        "  public class Inner4213 "
        "    public Y: int; "
        "  end Inner4213; "
        "  public method DoIt(): int => this.X; "
        "end extension; "
        "class CN4213 public static method Main(): int "
        "  return 0; "
        "end Main; end CN4213;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "an extension block may not declare a `nested type` member"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§26.3"));
}

// REJECT — EventDecl. Not named by the issue and not itself part of
// Appendix A.4's `Member` production in this spec revision (a separate,
// out-of-#4213-scope discrepancy — `event` is not documented anywhere in
// docs/Bezel_Language_Spec.md at all), but reachable via the SAME
// `parseMember()` an extension body's loop calls, and caught by the
// default-deny catch-all the same as any other non-`MethodDecl`/
// non-`PropertyDecl` kind. Found via this fix's own self-review testing:
// the catch-all's generic fallback labeled it "a `member` member" (the
// unnamed-kind default) before an explicit `EventDecl` arm was added to
// give it a proper "event" label — pinning the labeled text here catches a
// regression to the awkward fallback wording.
BEZEL_TEST(extension_member_kind_4213, extension_event_rejected_spec_26_3) {
    auto r = runAot(
        "module M; "
        "class WidgetE4213 "
        "  public X: int; "
        "end WidgetE4213; "
        "extension WidgetE4213 "
        "  public event Bell4213: int; "
        "  public method DoIt(): int => this.X; "
        "end extension; "
        "class CEv4213 public static method Main(): int "
        "  return 0; "
        "end Main; end CEv4213;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "an extension block may not declare an `event` member"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§26.3"));
}

// REJECT — IndexerDecl (a `PropertyDecl` with `isIndexer = true`; not named
// by the issue, but §26.3 is explicit: "No indexers."). Pre-fix this was
// NOT silently admitted — it reached `visitMember` (the existing
// body-kind check only screens for block-accessor/auto-property forms, and
// an indexer can be expression-bodied) and failed downstream with
// "unresolved identifier `i`" instead of naming the real rule. Asserting
// the SPECIFIC §26.3 text (not just "failed to compile") is what makes this
// a genuine FAIL-WITHOUT/PASS-WITH row despite both sides exiting non-zero.
BEZEL_TEST(extension_member_kind_4213,
           extension_indexer_rejected_spec_26_3) {
    auto r = runAot(
        "module M; "
        "class WidgetI4213 "
        "  public X: int; "
        "end WidgetI4213; "
        "extension WidgetI4213 "
        "  public property [i: int]: int => i; "
        "  public method DoIt(): int => this.X; "
        "end extension; "
        "class CI4213 public static method Main(): int "
        "  return 0; "
        "end Main; end CI4213;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "an extension block may not declare an indexer"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§26.3"));
}

// =====================================================================
// ACCEPT — legitimate extension bodies must keep compiling (LANE_RULES
// §15 — a new rejection must not become a false reject of what §26.3
// admits).
// =====================================================================

// ACCEPT — an ordinary extension on a class: method + expression-bodied
// property, no forbidden member kind. Unaffected by this fix.
BEZEL_TEST(extension_member_kind_4213,
           extension_method_and_property_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class WidgetOK4213 "
        "  public X: int; "
        "  public constructor(x: int) "
        "    X := x; "
        "  end constructor; "
        "end WidgetOK4213; "
        "extension WidgetOK4213 "
        "  public method DoIt(): int => this.X + 1; "
        "  public property Doubled: int => this.X * 2; "
        "end extension; "
        "class COK4213 public static method Main(): int "
        "  var w := new WidgetOK4213(20); "
        "  return w.DoIt() + w.Doubled; "
        "end Main; end COK4213;"),
        // DoIt() = 21, Doubled = 40 -> 61
        61);
}

// ACCEPT — an extension on a primitive type (§26.2 "Built-in primitive
// types"), method only: unaffected by this fix, and confirms the
// default-deny arm's `dynamic_cast` checks are purely AST-syntactic (no
// TypeSymbol dependency — a primitive extension's `currentType_` stays
// null, per the ExtensionDecl arm's own comment).
BEZEL_TEST(extension_member_kind_4213,
           extension_on_primitive_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "extension int "
        "  public method Triple4213(): int => this * 3; "
        "end extension; "
        "class CP4213 public static method Main(): int "
        "  var x: int := 14; "
        "  return x.Triple4213(); "
        "end Main; end CP4213;"),
        42);
}

// ACCEPT — a REGULAR class field, constructor, destructor, const, and
// nested type (NOT inside an extension) are entirely unaffected: this
// fix's new code lives only inside the ExtensionDecl arm of
// `ExprTC::visitTopDecl`.
BEZEL_TEST(extension_member_kind_4213,
           regular_class_members_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class Plain4213 "
        "  public X: int; "
        "  public static const K4213b: int := 1; "
        "  public class Inner4213b "
        "    public Y: int; "
        "  end Inner4213b; "
        "  public constructor(x: int) "
        "    X := x; "
        "  end constructor; "
        "  destructor "
        "  end destructor; "
        "end Plain4213; "
        "class CReg4213 public static method Main(): int "
        "  var p := new Plain4213(41); "
        "  return p.X + Plain4213.K4213b; "
        "end Main; end CReg4213;"),
        42);
}
