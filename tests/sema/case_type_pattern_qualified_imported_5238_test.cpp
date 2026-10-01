// Spec-derived tests for #5238: the #5188 "different value type" reject of a
// value-typed `case` scrutinee against a type pattern (Appendix C.1 "The same
// concrete operand or scrutinee against a value type other than its own or its
// own nullable form, not a type parameter, is a compile error", §12.7, §19.3,
// v0.313) must fire for a QUALIFIED name (`M.Qt`, `Lib.Qt`, an import alias
// `L.Qt`) and for a bare name of an IMPORTED type, not only for a bare local
// type name. The own type, however spelled, stays admitted.

#include <memory>
#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "lexer/Lexer.hpp"
#include "module/ModuleLoader.hpp"
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

ModuleMetadata buildMetaFor(std::string_view source) {
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

struct Run {
    DiagnosticEngine diags;
    std::unique_ptr<ast::SourceFile> sf;
    std::unique_ptr<sema::SemaContext> ctx;
    bool hadErrors = false;
    bool otherValueTypeError = false;
    std::string allErrors;
};

// `dep` null: single-module program. Otherwise the importer of `dep`.
std::unique_ptr<Run> run(const ModuleMetadata* dep, std::string_view src) {
    auto r = std::make_unique<Run>();
    Lexer lexer(src, r->diags);
    Parser parser(lexer, r->diags);
    r->sf = parser.parseCompilationUnit();
    if (!r->sf) { r->hadErrors = true; return r; }
    r->ctx = std::make_unique<sema::SemaContext>(r->diags);
    sema::collectSymbols(*r->sf, *r->ctx);
    if (dep) loadMetadata(*dep, *r->ctx);
    sema::resolveTypes(*r->sf, *r->ctx);
    sema::typecheckExpressions(*r->sf, *r->ctx);
    sema::checkDefiniteAssignment(*r->sf, *r->ctx);
    r->hadErrors = r->diags.hasErrors();
    for (const auto& d : r->diags.diagnostics()) {
        if (d.severity != Severity::Error) continue;
        r->allErrors += d.message + "\n";
        if (d.message.find("naming a different value type") !=
            std::string::npos)
            r->otherValueTypeError = true;
    }
    return r;
}

constexpr const char* kLib =
    "module Lib; "
    "public record Pt(x: int, y: int); "
    "public record Qt(a: int); "
    "public enum Col Red, Green; end Col; "
    "public choice Sh Circle(r: int), Sq(s: int); end Sh; "
    "public class U "
    "  public static method MkP(): Pt return new Pt(1, 2); end MkP; "
    "  public static method MkC(): Col return Col.Red; end MkC; "
    "  public static method MkS(): Sh return Sh.Circle(1); end MkS; "
    "end U;";

std::string local(const std::string& stmts) {
    return "module M; record Pt(x: int); record Qt(a: int); "
           "enum Col Red, Green; end Col; "
           "choice Sh Circle(r: int), Sq(s: int); end Sh; "
           "class C public static method Main(): int " +
           stmts + " end Main; end C;";
}

std::string imp(const std::string& header, const std::string& stmts) {
    return "module App; " + header +
           " class C public static method Main(): int " + stmts +
           " end Main; end C;";
}

}  // namespace

// ---- same-module qualified (`M.Qt`) --------------------------------------

BEZEL_TEST(case_type_pattern_qualified_imported_5238,
           same_module_qualified_other_record_rejected) {
    auto r = run(nullptr, local(
        "var p := new Pt(1); return (case p of M.Qt => 1, _ => 0);"));
    BEZEL_EXPECT_TRUE(r->otherValueTypeError);
}

BEZEL_TEST(case_type_pattern_qualified_imported_5238,
           same_module_qualified_other_enum_rejected) {
    auto r = run(nullptr, local(
        "var p := new Pt(1); return (case p of M.Col => 1, _ => 0);"));
    BEZEL_EXPECT_TRUE(r->otherValueTypeError);
}

BEZEL_TEST(case_type_pattern_qualified_imported_5238,
           same_module_qualified_own_record_admitted) {
    auto r = run(nullptr, local(
        "var p := new Pt(1); return (case p of M.Pt => 1, _ => 0);"));
    BEZEL_EXPECT_FALSE(r->hadErrors);
}

BEZEL_TEST(case_type_pattern_qualified_imported_5238,
           same_module_qualified_own_enum_and_choice_admitted) {
    auto r = run(nullptr, local(
        "var c := Col.Red; var s: Sh := Sh.Circle(1); "
        "return (case c of M.Col => 1, _ => 0) + "
        "(case s of M.Sh => 1, _ => 0);"));
    BEZEL_EXPECT_FALSE(r->hadErrors);
}

// ---- imported ------------------------------------------------------------

BEZEL_TEST(case_type_pattern_qualified_imported_5238,
           imported_qualified_other_record_rejected) {
    ModuleMetadata dep = buildMetaFor(kLib);
    auto r = run(&dep, imp("import Lib;",
        "var p := Lib.U.MkP(); return (case p of Lib.Qt => 1, _ => 0);"));
    BEZEL_EXPECT_TRUE(r->otherValueTypeError);
}

BEZEL_TEST(case_type_pattern_qualified_imported_5238,
           imported_bare_other_record_rejected) {
    ModuleMetadata dep = buildMetaFor(kLib);
    auto r = run(&dep, imp("import Lib;",
        "var p := Lib.U.MkP(); return (case p of Qt => 1, _ => 0);"));
    BEZEL_EXPECT_TRUE(r->otherValueTypeError);
}

BEZEL_TEST(case_type_pattern_qualified_imported_5238,
           imported_alias_qualified_other_record_rejected) {
    ModuleMetadata dep = buildMetaFor(kLib);
    auto r = run(&dep, imp("import Lib as L;",
        "var p := L.U.MkP(); return (case p of L.Qt => 1, _ => 0);"));
    BEZEL_EXPECT_TRUE(r->otherValueTypeError);
}

BEZEL_TEST(case_type_pattern_qualified_imported_5238,
           imported_enum_scrutinee_other_record_rejected) {
    ModuleMetadata dep = buildMetaFor(kLib);
    auto r = run(&dep, imp("import Lib;",
        "var c := Lib.U.MkC(); return (case c of Lib.Qt => 1, _ => 0);"));
    BEZEL_EXPECT_TRUE(r->otherValueTypeError);
}

BEZEL_TEST(case_type_pattern_qualified_imported_5238,
           imported_own_record_qualified_and_bare_admitted) {
    ModuleMetadata dep = buildMetaFor(kLib);
    auto r = run(&dep, imp("import Lib;",
        "var p := Lib.U.MkP(); "
        "return (case p of Lib.Pt => 1, _ => 0) + "
        "(case p of Pt => 1, _ => 0);"));
    BEZEL_EXPECT_FALSE(r->hadErrors);
}

BEZEL_TEST(case_type_pattern_qualified_imported_5238,
           imported_own_alias_record_admitted) {
    ModuleMetadata dep = buildMetaFor(kLib);
    auto r = run(&dep, imp("import Lib as L;",
        "var p := L.U.MkP(); return (case p of L.Pt => 1, _ => 0);"));
    BEZEL_EXPECT_FALSE(r->hadErrors);
}

BEZEL_TEST(case_type_pattern_qualified_imported_5238,
           imported_own_enum_and_choice_admitted) {
    ModuleMetadata dep = buildMetaFor(kLib);
    auto r = run(&dep, imp("import Lib;",
        "var c := Lib.U.MkC(); var s := Lib.U.MkS(); "
        "return (case c of Lib.Col => 1, _ => 0) + "
        "(case c of Col => 1, _ => 0) + "
        "(case s of Lib.Sh => 1, _ => 0) + "
        "(case s of Sh => 1, _ => 0);"));
    BEZEL_EXPECT_FALSE(r->hadErrors);
}

// An enum constant spelled through the module/type qualifier is a constant
// pattern, not a type pattern, and stays admitted.
BEZEL_TEST(case_type_pattern_qualified_imported_5238,
           imported_qualified_enum_constant_admitted) {
    ModuleMetadata dep = buildMetaFor(kLib);
    auto r = run(&dep, imp("import Lib;",
        "var c := Lib.U.MkC(); "
        "return (case c of Lib.Col.Red => 1, _ => 0);"));
    BEZEL_EXPECT_FALSE(r->hadErrors);
}

}  // namespace bezel::module
