// fix #3629 (§22.15 / §11.8 / §2.8.2, v0.286): `pure` on an interface
// `static operator` was silently DROPPED — every other interface-member shape
// (IfaceProperty/IfaceIndexer pushed "pure" into a modifier list at line 1677,
// IfaceMethod/IfaceStaticMethod at 1800, IfaceEvent reports a §22.15 hard reject
// at 1758) consumed `pureMod`, but IfaceOperator built an `ast::OperatorDecl`
// and never set the dedicated `op->isPure` bool. The symbol then read
// `MemberSymbol::isPure == false`, the wire wrote `MetaMember::isPure == false`,
// and an importer saw an unpure operator even though the source declared it
// `pure`. A generic `pure` body doing `a + b` on `where T: INum<T>` therefore
// got §22.15 rule 6's IMPURE rejection, with a "declare it `pure`" suggestion
// — that suggestion is exactly what the source already did.
//
// WHAT THIS FILE PINS. The parser half is the only half needing a code change;
// the symbol side already channels through `collectMember` -> `declaredPure` ->
// `od->isPure` (SymbolCollection.cpp:344), and the wire side already writes
// `MetaMember::isPure = od->isPure` (ModuleMetadataBuilder.cpp:1227). So the
// rows verify:
//
//   T1  AST + symbol: an interface's `pure static operator +` builds an
//       `ast::OperatorDecl` whose `isPure` is true, AND its `MethodSymbol`'s
//       `MemberSymbol::isPure` (set by `collectMember` from the AST) is true,
//       AND `sema::memberIsPure` answers true. This is the row that FAILS
//       WITHOUT the fix: pre-fix the AST `isPure` is false, the symbol field
//       is false, and `memberIsPure` answers false through the `od->isPure` arm
//       and the empty modifier list.
//   T2  Cross-module twin: the LOCAL symbol and the IMPORTED symbol agree on
//       `MemberSymbol::isPure` for the interface operator. Pre-fix this row
//       also fails: local reads false, importer reads false too (the wire
//       faithfully serialised the parser's omission), but they agree for the
//       WRONG reason — pin that the agreement is on `true`.
//   T3  §22.15 VERDICT CONTROL. A pure body calling `T.+` through
//       `where T: INum<T>` is ADMITTED when the interface operator is pure
//       and REJECTED when it is not. The unmarked twin is the over-acceptance
//       control — the predicate must NOT just always say yes. This is the
//       behavior the bug report measured end-to-end.
//
// Spec: §22.15 "The pure body" rule 6, §22.15 "Where `pure` may appear",
// §11.8 static interface members, §2.8.2 the artifact interface, §25 operator
// overloading. All v0.286. Related: #3610 (the rule-6 EXPRESSION arm,
// fixed in #3610/#3628), #3609 (`memberIsPure` single authority), #3618 (the
// `pure get` wire slot, separate family).

#include "module/ModuleLoader.hpp"
#include "module/ModuleMetadata.hpp"
#include "module/ModuleMetadataBuilder.hpp"

#include <memory>
#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/PureChecker.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

// Source for the EXPORTER: an interface declares `pure static operator +` (the
// bug surface), and a record implements it with `public static pure method
// operator +` (the same source shape `parseOperatorDeclBody` already folds
// `pure` from — the class-side twin the parser got right). One interface, one
// record; the operators are matched on token, name, arity, and return type, and
// the `pure` markers are the only asymmetry between the bug row and its
// unmarked control (T3).
constexpr std::string_view kPureSrc =
    "module PureLib; "
    "public interface INum<T> where T: INum<T> "
    "  pure static operator +(a: T, b: T): T; "
    "end INum; "
    "public record Num(V: int) : INum<Num> "
    "  public static pure method operator +(a: Num, b: Num): Num => a; "
    "end Num;";

// Source for the IMPURE control: identical source with the `pure` marker
// removed from the INTERFACE operator only. The class-side `pure method
// operator` is unchanged — that path was never broken.
constexpr std::string_view kImpureSrc =
    "module ImpureLib; "
    "public interface INum<T> where T: INum<T> "
    "  static operator +(a: T, b: T): T; "
    "end INum; "
    "public record Num(V: int) : INum<Num> "
    "  public static pure method operator +(a: Num, b: Num): Num => a; "
    "end Num;";

// Importer's source: a generic `pure` body that calls `+` on a constraint-typed
// Two-step front end: collect symbols + type-resolve + typecheck. Same order
// Driver.cpp runs.
struct Exported {
    DiagnosticEngine diagnostics;
    std::unique_ptr<ast::SourceFile> sourceFile;
    std::unique_ptr<sema::SemaContext> ctx;
    ModuleMetadata meta;
    bool ok = false;
};

std::unique_ptr<Exported> exportModule(std::string_view source) {
    auto r = std::make_unique<Exported>();
    Lexer lexer(source, r->diagnostics);
    Parser parser(lexer, r->diagnostics);
    r->sourceFile = parser.parseCompilationUnit();
    if (!r->sourceFile) return r;
    r->ctx = std::make_unique<sema::SemaContext>(r->diagnostics);
    sema::collectSymbols(*r->sourceFile, *r->ctx);
    sema::resolveTypes(*r->sourceFile, *r->ctx);
    sema::typecheckExpressions(*r->sourceFile, *r->ctx);
    r->meta = buildModuleMetadata(*r->ctx, source);
    r->ok = !r->diagnostics.hasErrors();
    return r;
}

// The LOCAL `INum` TypeSymbol. Operators are keyed by their token
// (SymbolCollection.cpp:203), so `member(t, "+")` recovers the right one.
const sema::TypeSymbol* localINum(const Exported& e) {
    if (!e.ctx || !e.ctx->module()) return nullptr;
    for (sema::Symbol* c : e.ctx->module()->children)
        if (auto* t = dynamic_cast<sema::TypeSymbol*>(c))
            if (t->name == "INum") return t;
    return nullptr;
}

const sema::MemberSymbol* member(const sema::TypeSymbol* t,
                                 std::string_view name) {
    if (!t) return nullptr;
    for (sema::Symbol* m : t->members)
        if (auto* mem = dynamic_cast<sema::MemberSymbol*>(m))
            if (mem->name == name) return mem;
    return nullptr;
}

// Run the importer's front end (no PureChecker — T3 drives it) and report
// whether PureChecker's §22.15 rule 6 message fired. The search is over
// `Error`-severity diagnostics with the exact "not admitted in a pure body"
// substring — the wording the issue body cites, so a fix that silently flipped
// the verdict without naming it would NOT pass.
//
// `dep` is the loaded module metadata; the importer's `import` directive names
// the same module the exporter used (the importer is `module App; import X`).
// The exporter's module name is recovered from its module declaration header
// (one source per call — the literal "PureLib"/"ImpureLib" the exporter srcs
// declare), so the helper takes both as separate parameters.
bool rejectsAsImpureCall(const ModuleMetadata& dep,
                         std::string_view exporterModuleName,
                         std::string_view importerModuleName) {
    DiagnosticEngine diags;
    std::string src;
    src.reserve(128);
    src += "module ";
    src += std::string(importerModuleName);
    src += "; import ";
    src += std::string(exporterModuleName);
    src += "; public class Program "
           "public static pure method Sum<T>(a: T, b: T): T where T: INum<T> "
           "return a + b; end Sum; "
           "public static method Main(): int => 0; "
           "end Program;";
    Lexer lexer(src, diags);
    Parser parser(lexer, diags);
    std::unique_ptr<ast::SourceFile> srcFile = parser.parseCompilationUnit();
    if (!srcFile) return false;
    sema::SemaContext ctx(diags);
    sema::collectSymbols(*srcFile, ctx);
    loadMetadata(dep, ctx);
    sema::resolveTypes(*srcFile, ctx);
    sema::typecheckExpressions(*srcFile, ctx);
    sema::checkPureBodies(*srcFile, ctx);
    for (const auto& d : diags.diagnostics())
        if (d.severity == Severity::Error &&
            d.message.find("not admitted in a pure body") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// =========================================================================
// T1 — AST + SYMBOL half: a `pure static operator +` on an interface produces
// an `ast::OperatorDecl` with `isPure = true`, a `MethodSymbol` with
// `MemberSymbol::isPure = true`, and `memberIsPure` answering true. This is
// the row that FAILS WITHOUT THE FIX: pre-fix the AST bool, the symbol field,
// and the predicate all read false (the predicate falls through to an empty
// modifier list).
// =========================================================================
BEZEL_TEST(pure_iface_static_operator_3629,
           parser_preserves_pure_on_iface_static_operator_spec_22_15) {
    auto e = exportModule(kPureSrc);
    BEZEL_EXPECT_TRUE(e->ok);
    const sema::TypeSymbol* inum = localINum(*e);
    BEZEL_EXPECT_TRUE(inum != nullptr);
    const sema::MemberSymbol* plus = member(inum, "+");
    BEZEL_EXPECT_TRUE(plus != nullptr);
    if (!plus) return;

    // The FIELD: pre-fix this is false. The symbol-side channel is set by
    // `collectMember` -> `declaredPure` from `ast::OperatorDecl::isPure`
    // (SymbolCollection.cpp:344-345), so it stays false until the parser
    // assigns the AST bool.
    BEZEL_EXPECT_TRUE(plus->isPure);

    // The AST DECL ARM of `memberIsPure`: the predicate must read the AST bool
    // directly (the comment at Symbol.hpp:1319-1325 explains why — built
    // before the field is assigned for a local declaration).
    const ast::OperatorDecl* opDecl =
        dynamic_cast<const ast::OperatorDecl*>(plus->decl);
    BEZEL_EXPECT_TRUE(opDecl != nullptr);
    if (opDecl) {
        BEZEL_EXPECT_TRUE(opDecl->isPure);
    }

    // The PREDICATE: every §22.15 site asks here. Pre-fix this answers false
    // through both the `od->isPure` arm (false) and the empty modifier list.
    BEZEL_EXPECT_TRUE(sema::memberIsPure(plus));
}

// =========================================================================
// T2 — CROSS-MODULE: the LOCAL interface-operator symbol and the symbol
// reconstructed from the SAME module's own artifact interface must agree on
// `MemberSymbol::isPure`. Pre-fix both sides read false for the WRONG reason
// (the wire faithfully serialised the parser's omission); pin that they now
// agree on `true`.
// =========================================================================
BEZEL_TEST(pure_iface_static_operator_3629,
           imported_and_local_twin_agree_on_iface_pure_operator_spec_2_8_2) {
    auto e = exportModule(kPureSrc);
    BEZEL_EXPECT_TRUE(e->ok);
    DiagnosticEngine iDiags;
    constexpr std::string_view kImp = "module App; import PureLib; "
        "public class Program public static method Main(): int => 0; "
        "end Program;";
    Lexer ilexer(kImp, iDiags);
    Parser iparser(ilexer, iDiags);
    std::unique_ptr<ast::SourceFile> isrc = iparser.parseCompilationUnit();
    BEZEL_EXPECT_TRUE(isrc != nullptr);
    sema::SemaContext ictx(iDiags);
    sema::collectSymbols(*isrc, ictx);
    BEZEL_EXPECT_TRUE(loadMetadata(e->meta, ictx));
    sema::resolveTypes(*isrc, ictx);
    sema::typecheckExpressions(*isrc, ictx);
    BEZEL_EXPECT_TRUE(!iDiags.hasErrors());

    const sema::TypeSymbol* localT = localINum(*e);
    const sema::TypeSymbol* impT = ictx.lookupImportedTypeByName("INum");
    BEZEL_EXPECT_TRUE(localT != nullptr);
    BEZEL_EXPECT_TRUE(impT != nullptr);
    if (!localT || !impT) return;

    const sema::MemberSymbol* lm = member(localT, "+");
    const sema::MemberSymbol* im = member(impT, "+");
    BEZEL_EXPECT_TRUE(lm != nullptr);
    BEZEL_EXPECT_TRUE(im != nullptr);
    if (!lm || !im) return;

    BEZEL_EXPECT_TRUE(im->isPure);
    BEZEL_EXPECT_EQ(lm->isPure, im->isPure);
    BEZEL_EXPECT_EQ(sema::memberIsPure(lm), sema::memberIsPure(im));
}

// =========================================================================
// T3 — §22.15 BEHAVIOUR CONTROL. The end-to-end verdict: a generic `pure`
// body calling `a + b` on `where T: INum<T>` is ADMITTED when the interface
// operator is `pure` and REJECTED when it is not. The unmarked twin is the
// over-acceptance control — a "predicate that always says yes" predicate
// would pass the pure half but fail this.
// =========================================================================
BEZEL_TEST(pure_iface_static_operator_3629,
           rule_6_verdict_matches_iface_operator_marker_spec_22_15) {
    auto pureExp = exportModule(kPureSrc);
    auto impureExp = exportModule(kImpureSrc);
    BEZEL_EXPECT_TRUE(pureExp->ok);
    BEZEL_EXPECT_TRUE(impureExp->ok);

    // Pure interface operator: rule 6 admits the call.
    BEZEL_EXPECT_TRUE(!rejectsAsImpureCall(pureExp->meta, "PureLib", "App"));
    // Unmarked interface operator: rule 6 still rejects (the over-acceptance
    // control — without it the pure half above would pass vacuously).
    BEZEL_EXPECT_TRUE(rejectsAsImpureCall(impureExp->meta, "ImpureLib", "App"));
}

}  // namespace bezel::module