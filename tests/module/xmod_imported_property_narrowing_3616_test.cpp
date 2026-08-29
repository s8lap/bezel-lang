// fix #3616 (§7.4 / §2.8.3 rule 4, v0.286): §7.4's mutably-captured-narrowing
// invalidation was silently skipped for EVERY imported property, so the same
// program was rejected with a local class and accepted with an imported one.
//
// THE RULE (§7.4, #1566/#1593). While a mutably-captured local is narrowed
// non-null, any operation in the controlled block that can transfer control to
// user code — a method/constructor/ACCESSOR call, or an `await` — may reach the
// capturing closure and null the local, so it DROPS the narrowing. A
// compiler-generated backing-field load (a non-virtual auto-property) cannot run
// user code and does not drop it. `propertyAccessRunsUserCode`
// (ExprTypecheck_Exprs.cpp) is that classification.
//
// THE DEFECT. The predicate took an `ast::PropertyDecl*` and answered `false` for
// null, and its three call sites had drifted into three different spellings of
// the null case:
//   * the named-property site and the generic-constraint-indexer site wrote
//     `if (auto* pd = dynamic_cast<...>(ps->decl)) if (propertyAccessRunsUserCode(pd))`
//     — an imported property (§2.5 reconstructs it with `decl == nullptr`) skipped
//     the invalidation entirely;
//   * the indexer site wrote `pd ? propertyAccessRunsUserCode(pd) : true`, with a
//     comment explaining that unknown must be conservative.
// The predicate's own comment called leaving an imported property alone
// "conservative". For an INVALIDATION rule it is the opposite: the narrowing
// SURVIVES an accessor that may null the local, which is what §7.4 exists to
// forbid. §2.8.3 rule 4 requires the two artifacts to agree.
//
// MEASURED (before the fix, on `main` @ 907ce0f) — the SAME body, with `Box`
// declared locally vs imported:
//   local    -> rejected: "cannot narrow `T?` accessed through a mutably-captured
//               local (§7.4): copy to an uncaptured local first"
//   imported -> ACCEPTED, no diagnostic.
// The program is `tests/lit/sema/sema_null_narrow__expr_body_property_getter_
// invalidates_capture_spec_7_4.bzl` with the `Box` declaration moved across a
// module boundary and nothing else changed.
//
// THE FIX — ONE ACCESSOR, NOT ONE BLANKET DEFAULT. A second overload takes the
// SYMBOL and is the only one the call sites use; the AST-only overload is now an
// implementation detail of its local arm. It could have answered a blunt `true`
// for every decl-less property — and that would have been WRONG in the other
// direction: the sibling lit test `sema_null_narrow__auto_property_read_keeps_
// capture_narrowing_spec_7_4.bzl` pins that a NON-VIRTUAL AUTO property read must
// NOT invalidate, and an imported one is still a plain backing-field load. So the
// imported arm asks the existing both-sources classification
// (`sema::isAutoPropertyStorageSymbol`, sema/Symbol.hpp — #2478's wire bit,
// `bodyKind == Auto && !static && !abstract`) plus the wire-carried `modifiers`,
// and REPRODUCES the local answer instead of approximating it. Both directions
// are pinned below.
//
// §6b EXCLUSION DIFF — what the unified predicate ADMITS that the old spellings
// EXCLUDED. Only decl-less (== imported, §2.5) PropertySymbols change answer, and
// within those:
//   * imported EXPRESSION- or BLOCK-bodied property        false -> true  (T1)
//   * imported `virtual`/`abstract`/`override` auto-property false -> true
//   * imported indexer                                      false -> true (already
//     `true` at the one site that had it right; now true at all three)
//   * imported plain auto-property                          false -> false (T2)
//   * imported STATIC auto-property                         false -> true  — the one
//     deliberate over-conservatism: the #2478 wire bit is cleared for `static`, and
//     nothing else the import carries separates a static auto body from a static
//     expression body. It costs a narrowing optimization on a shape that must ALSO
//     sit inside a mutating-capture block to be observable; never unsound.
// A local property is untouched: the symbol overload delegates to the identical
// AST body on the `ps->decl != nullptr` path.
// The outer `dynamic_cast<PropertySymbol*>` gate at the named-property site is
// deliberately KEPT: a builtin `.Length`/`.Count` access sets no PropertySymbol
// at all and is not this rule's business — widening to it would invalidate a
// narrowing on every `.Length` read.
//
// The whole rule is a no-op unless the enclosing body has a lambda that MUTATES a
// captured local (`invalidateCapturedNarrowings` returns immediately when
// `mutablyCapturedNames_` is empty), which bounds the blast radius of the change
// to exactly the shape T1 exercises.
//
// This is a SEMA-level observable, so it runs the front-end in process against
// loaded metadata (the `cross_module_import_test.cpp` harness shape) and asserts
// the diagnostic TEXT, not `hasErrors()`.

#include "module/ModuleLoader.hpp"

#include <memory>
#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "lexer/Lexer.hpp"
#include "module/ModuleMetadata.hpp"
#include "module/ModuleMetadataBuilder.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

ModuleMetadata buildMeta3616(std::string_view source) {
    DiagnosticEngine diags;
    Lexer lexer(source, diags);
    Parser parser(lexer, diags);
    auto sf = parser.parseCompilationUnit();
    if (!sf) return {};
    sema::SemaContext ctx(diags);
    sema::collectSymbols(*sf, ctx);
    sema::resolveTypes(*sf, ctx);
    sema::typecheckExpressions(*sf, ctx);
    return buildModuleMetadata(ctx, source);
}

struct Run3616 {
    DiagnosticEngine diags;
    std::unique_ptr<ast::SourceFile> sf;
    std::unique_ptr<sema::SemaContext> ctx;
    bool parsed = false;
};

// Front-end over `src`; when `dep` is non-null its interface is loaded first, so
// the SAME importer body can be run against an imported or a local declaration.
std::unique_ptr<Run3616> run3616(const ModuleMetadata* dep,
                                 std::string_view src) {
    auto r = std::make_unique<Run3616>();
    Lexer lexer(src, r->diags);
    Parser parser(lexer, r->diags);
    r->sf = parser.parseCompilationUnit();
    if (!r->sf) return r;
    r->parsed = true;
    r->ctx = std::make_unique<sema::SemaContext>(r->diags);
    sema::collectSymbols(*r->sf, *r->ctx);
    if (dep) loadMetadata(*dep, *r->ctx);
    sema::resolveTypes(*r->sf, *r->ctx);
    sema::typecheckExpressions(*r->sf, *r->ctx);
    sema::checkDefiniteAssignment(*r->sf, *r->ctx);
    return r;
}

// True iff SOME diagnostic's message contains `needle`. Asserting the TEXT (not
// `hasErrors()`) is what makes this test able to distinguish §7.4's rejection
// from any other error the program might acquire later.
bool sawDiag3616(const Run3616& r, std::string_view needle) {
    for (const auto& d : r.diags.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// The §7.4 diagnostic, verbatim from ExprTypecheck (also the CHECK line of
// tests/lit/sema/sema_null_narrow__*_spec_7_4.bzl).
constexpr const char* kNarrowDiag3616 =
    "cannot narrow `T?` accessed through a mutably-captured local";

// The dependency: an EXPRESSION-bodied property (runs user code) and a plain
// non-virtual AUTO property (a backing-field load) on two classes, so one
// metadata blob serves both directions.
const char* kDecls3616 =
    "public class Box "
    "  public property Val: int => 42; "
    "end Box; "
    "public class ABox "
    "  public property AV: int := 7; "
    "end ABox; ";

std::string lib3616() { return std::string("module Lib; ") + kDecls3616; }

// The importer body — character-identical between the imported and local runs.
// It is `sema_null_narrow__expr_body_property_getter_invalidates_capture_spec_7_4`
// with the read parameterised.
std::string body3616(const std::string& paramTy, const std::string& read) {
    return "class C "
           "  public static method Run(box: " + paramTy + "): int "
           "    var s: string? := \"hi\"; "
           "    var f := lambda() => do s := null; end; "
           "    if s <> null then "
           "      _ := " + read + "; "
           "      return s.Length; "
           "    end; "
           "    return -1; "
           "  end Run; "
           "end C; ";
}

std::string app3616(const std::string& paramTy, const std::string& read) {
    return "module App; import Lib; " + body3616(paramTy, read);
}

std::string local3616(const std::string& paramTy, const std::string& read) {
    return std::string("module App; ") + kDecls3616 + body3616(paramTy, read);
}

}  // namespace

// #3616 / §7.4 + §2.8.3 rule 4 — THE DEFECT. An IMPORTED expression-bodied
// property read between the null test and the use must drop the narrowing.
// Pre-fix: accepted, no diagnostic.
BEZEL_TEST(xmod_imported_property_narrowing_3616,
           imported_expr_body_property_invalidates_capture_spec_7_4) {
    ModuleMetadata dep = buildMeta3616(lib3616());
    auto r = run3616(&dep, app3616("Box", "box.Val"));
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(sawDiag3616(*r, kNarrowDiag3616));
}

// #3616 — the single-module CONTROL for the same body. Rejected on BOTH sides of
// the fix; §2.8.3 rule 4 is the requirement that these two agree, so the test is
// only meaningful as a pair.
BEZEL_TEST(xmod_imported_property_narrowing_3616,
           local_expr_body_property_invalidates_capture_spec_7_4) {
    auto r = run3616(nullptr, local3616("Box", "box.Val"));
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(sawDiag3616(*r, kNarrowDiag3616));
}

// #3616 — THE OVER-REJECTION CONTROL, and the reason the fix is an accessor and
// not a blanket conservative default. An IMPORTED NON-VIRTUAL AUTO property is a
// compiler-generated backing-field load: it cannot run user code, so it must NOT
// drop the narrowing. A `return true` for every decl-less property would pass the
// two tests above and FAIL here.
BEZEL_TEST(xmod_imported_property_narrowing_3616,
           imported_auto_property_keeps_capture_narrowing_spec_7_4) {
    ModuleMetadata dep = buildMeta3616(lib3616());
    auto r = run3616(&dep, app3616("ABox", "box.AV"));
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(!sawDiag3616(*r, kNarrowDiag3616));
    BEZEL_EXPECT_TRUE(!r->diags.hasErrors());
}

// #3616 — the auto-property control's single-module twin (the shape
// tests/lit/sema/sema_null_narrow__auto_property_read_keeps_capture_narrowing_
// spec_7_4.bzl pins). Accepted on both sides; pairs with the test above exactly
// as the two rejection tests pair.
BEZEL_TEST(xmod_imported_property_narrowing_3616,
           local_auto_property_keeps_capture_narrowing_spec_7_4) {
    auto r = run3616(nullptr, local3616("ABox", "box.AV"));
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(!sawDiag3616(*r, kNarrowDiag3616));
    BEZEL_EXPECT_TRUE(!r->diags.hasErrors());
}

}  // namespace bezel::module
