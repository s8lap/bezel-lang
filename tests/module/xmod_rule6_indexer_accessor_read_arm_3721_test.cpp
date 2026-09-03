// §22.15 rule 6 accessor-read arm — the INDEXER twin of fix #3630 / #3618
// (tests/module/xmod_rule6_accessor_read_arm_3618_3630_test.cpp).
//
// WHAT THE DEFECT WAS. §22.15 rule 6 names the accessor-read arm as one of
// the four "calls in the admitted positions": "A call — method, operator,
// **accessor read**, or constructor ... is permitted iff the member
// declared on the receiver's static type ... is `pure`-declared or
// pure-classified." Fix #3630/#3618 closed this for NAMED property reads
// (`c.Value`, a MemberAccessExpr) but explicitly left indexer reads
// (`obj[i]`, an IndexExpr) open — #3630's own commit body: "Indexers
// (`obj[i]`) are NOT covered here ... filed separately for indexers." That
// filing never happened until #3721 (this fix's issue): PureChecker.cpp's
// IndexExpr arm only descended into the target and index expressions, so a
// non-`pure` indexer `get` accessor's side effects ran unobserved inside a
// `pure` body (measured: compile exit 0, run exit 4 — the getter's
// `Calls := Calls + 1` executed inside `pure P`).
//
// WHY THIS WAS A ONE-SIDED FIX (PureChecker.cpp only). #3721's own
// structural note claimed "no writer ever records a resolved symbol on an
// ast::IndexExpr node" — REFUTED by this lane: ExprTypecheck_Exprs.cpp's
// `visitExpr` IndexExpr arm already calls `ctx_.setExprSymbol(&e, ps)` (fix
// #2530, §22.3/§23) for the user-declared-indexer branch, for BOTH a local
// and an imported indexer (the `tt->declaration->members` walk there
// resolves both identically — #2414's comment records the imported-
// candidate fix). And the purity classification itself is already
// both-sources: `PropertySymbol::accessorIsPure` (Symbol.hpp) is populated
// identically for a Property or an Indexer by SymbolCollection.cpp's single
// `PropertyDecl` arm (`pd->isIndexer` is just a bit on the same node)
// locally, and by ModuleLoader.cpp's Indexer case (`p->accessorIsPure =
// mm.accessorIsPure`, mirroring the Property case) across the module
// boundary — ModuleMetadataBuilder.cpp's exporter walk for
// `MetaMember::accessorIsPure` (#3618) already says explicitly "Covers both
// Property AND Indexer." So `propertyGetterIsPure`, the SAME authority the
// MemberAccessExpr arm asks, already answered the indexer question
// correctly on both sides of the module boundary before this fix — the
// IndexExpr arm in PureChecker.cpp's `checkExpr` just never asked it. T4
// below re-verifies the wire carriage specifically through the INDEXER
// reconstruction branch (a different ModuleLoader.cpp arm than the Property
// one #3618's own T4 pinned), since the two are separate code paths that
// could drift independently.
//
// WHAT THIS FILE PINS:
//   T1  the LOCAL REJECT (#3721): a non-`pure` indexer getter that writes a
//       static field is rejected when read inside a `pure` body. Row that
//       FAILS WITHOUT the fix.
//   T2  the LOCAL `pure get` indexer ADMIT (control): the accessor-level
//       `pure get` marker on an indexer is admitted — T1+T2 together prove
//       a real reject, not a blanket one.
//   T3  the BUILTIN-COLLECTION ADMIT (control, §6b): `Array<T>`'s `[i]`
//       resolves no user `PropertySymbol` at the ExprTypecheck_Exprs.cpp
//       site (gated on `tt->kind == Named && tt->declaration` with a
//       matching indexer candidate), so `ctx_.exprSymbol(*e)` stays null
//       and the new arm is a no-op — this row proves the fix does not
//       false-reject the overwhelmingly common builtin-collection index
//       read in a pure body.
//   T4  the WIRE EXPORT for an INDEXER (#3721 re-verification of #3618's
//       carriage through the Indexer branch specifically). Row that FAILS
//       WITHOUT the fix if the Indexer wire branch had drifted from the
//       Property one (it does not — this is a regression pin, not a new
//       defect).
//   T5  the CROSS-MODULE ADMIT: the imported indexer's `pure get` getter
//       is admitted by an importer-side `pure` body.
//   T6  the CROSS-MODULE NON-PURE REJECT (control for T5): an imported
//       non-`pure` block-form indexer getter is rejected inside a `pure`
//       body.
//
// Spec: §22.15 "The pure body" rule 6 / "Where `pure` may appear" /
// "Purity attaches to the getter", §23.4 indexers, §2.8.2 "the artifact
// interface", v0.286. Issue #3721; related #3610, #3618, #3630.

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

// ---- Shared exporter / importer harness (mirrors #3618/#3630's file) ------

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

struct Imported {
    DiagnosticEngine diagnostics;
    std::unique_ptr<ast::SourceFile> sourceFile;
    std::unique_ptr<sema::SemaContext> ctx;
    bool ok = false;
};

std::unique_ptr<Imported> importModuleInto(const ModuleMetadata& dep,
                                           std::string_view importerSrc) {
    auto r = std::make_unique<Imported>();
    Lexer lexer(importerSrc, r->diagnostics);
    Parser parser(lexer, r->diagnostics);
    r->sourceFile = parser.parseCompilationUnit();
    if (!r->sourceFile) return r;
    r->ctx = std::make_unique<sema::SemaContext>(r->diagnostics);
    sema::collectSymbols(*r->sourceFile, *r->ctx);
    r->ok = loadMetadata(dep, *r->ctx);
    sema::resolveTypes(*r->sourceFile, *r->ctx);
    sema::typecheckExpressions(*r->sourceFile, *r->ctx);
    return r;
}

const sema::TypeSymbol* localType(const Exported& e, std::string_view name) {
    if (!e.ctx || !e.ctx->module()) return nullptr;
    for (sema::Symbol* c : e.ctx->module()->children)
        if (auto* t = dynamic_cast<sema::TypeSymbol*>(c))
            if (t->name == name) return t;
    return nullptr;
}

const sema::TypeSymbol* importedType(const Imported& i, std::string_view name) {
    if (!i.ctx) return nullptr;
    return i.ctx->lookupImportedTypeByName(name);
}

// An indexer's PropertySymbol::name is the synthetic ".this" (SymbolCollection
// .cpp's PropertyDecl arm), so — unlike the named-property `member()` helper
// in the sibling #3618/#3630 file — this looks up the (sole) indexer by
// `isIndexer` rather than by name.
const sema::PropertySymbol* indexerOf(const sema::TypeSymbol* t) {
    if (!t) return nullptr;
    for (sema::Symbol* m : t->members)
        if (auto* ps = dynamic_cast<sema::PropertySymbol*>(m))
            if (ps->isIndexer) return ps;
    return nullptr;
}

// Run front-end + PureChecker over `importerSrc` with `dep` loaded, returning
// whether an "is not admitted in a pure body" diagnostic fired.
bool rejectsAsImpureAccessorCall(const ModuleMetadata& dep,
                                 std::string_view importerSrc) {
    DiagnosticEngine diags;
    Lexer lexer(importerSrc, diags);
    Parser parser(lexer, diags);
    std::unique_ptr<ast::SourceFile> src = parser.parseCompilationUnit();
    if (!src) return false;
    sema::SemaContext ctx(diags);
    sema::collectSymbols(*src, ctx);
    loadMetadata(dep, ctx);
    sema::resolveTypes(*src, ctx);
    sema::typecheckExpressions(*src, ctx);
    sema::checkPureBodies(*src, ctx);
    for (const auto& d : diags.diagnostics())
        if (d.severity == Severity::Error &&
            d.message.find("not admitted in a pure body") != std::string::npos)
            return true;
    return false;
}

// LOCAL (no imports) twin of the above.
bool localRejectsAsImpureAccessor(std::string_view src) {
    DiagnosticEngine diags;
    Lexer lexer(src, diags);
    Parser parser(lexer, diags);
    std::unique_ptr<ast::SourceFile> sf = parser.parseCompilationUnit();
    if (!sf) return false;
    sema::SemaContext ctx(diags);
    sema::collectSymbols(*sf, ctx);
    sema::resolveTypes(*sf, ctx);
    sema::typecheckExpressions(*sf, ctx);
    sema::checkPureBodies(*sf, ctx);
    for (const auto& d : diags.diagnostics())
        if (d.severity == Severity::Error &&
            d.message.find("not admitted in a pure body") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// =========================================================================
// T1 — the LOCAL REJECT (#3721). The issue's own repro shape: a non-`pure`
//       indexer getter that WRITES a static field, read inside a `pure`
//       body. FAILS WITHOUT the fix (the IndexExpr arm descended only into
//       target + indices, never checking the resolved accessor).
// =========================================================================
BEZEL_TEST(xmod_rule6_indexer_accessor_read_arm, non_pure_indexer_getter_in_pure_body_rejected_3721) {
    constexpr std::string_view kLocalSrc =
        "module App; "
        "public class Bag "
        "  public static Calls: int := 0; "
        "  private _n: int := 7; "
        "  public property [i: int]: int "
        "    get => do Calls := Calls + 1; return i + _n; end; "
        "  end; "
        "end Bag; "
        "public class Program "
        "  public static pure method P(b: Bag): int "
        "    return b[3]; "
        "  end P; "
        "  public static method Main(): int => 0; "
        "end Program;";
    BEZEL_EXPECT_TRUE(localRejectsAsImpureAccessor(kLocalSrc));
}

// =========================================================================
// T2 — the LOCAL `pure get` indexer ADMIT (control). T1+T2 together prove
//       a real reject rather than a blanket one.
// =========================================================================
BEZEL_TEST(xmod_rule6_indexer_accessor_read_arm, pure_get_indexer_admitted_in_pure_body_3721) {
    constexpr std::string_view kLocalSrc =
        "module App; "
        "public class Bag "
        "  public property [i: int]: int "
        "    pure get => i + 1; "
        "  end; "
        "end Bag; "
        "public class Program "
        "  public static pure method P(b: Bag): int "
        "    return b[3]; "
        "  end P; "
        "  public static method Main(): int => 0; "
        "end Program;";
    BEZEL_EXPECT_FALSE(localRejectsAsImpureAccessor(kLocalSrc));
}

// =========================================================================
// T3 — the BUILTIN-COLLECTION ADMIT (control, §6b mandatory exclusion
//       diff). `Array<T>`'s `[i]` resolves no user PropertySymbol at the
//       typing site (the ExprTypecheck_Exprs.cpp branch that calls
//       `setExprSymbol` is gated on a NAMED receiver type with a matching
//       user indexer candidate), so this arm is a no-op here — proves the
//       fix does not false-reject the common builtin-index-in-pure-body
//       shape.
// =========================================================================
BEZEL_TEST(xmod_rule6_indexer_accessor_read_arm, builtin_array_index_admitted_in_pure_body_3721_control) {
    constexpr std::string_view kLocalSrc =
        "module App; "
        "public class Program "
        "  public static pure method P(a: Array<int>): int "
        "    return a[0]; "
        "  end P; "
        "  public static method Main(): int => 0; "
        "end Program;";
    BEZEL_EXPECT_FALSE(localRejectsAsImpureAccessor(kLocalSrc));
}

// =========================================================================
// T4 — the WIRE EXPORT for an INDEXER. Re-verifies #3618's
//       `MetaMember::accessorIsPure` carriage through the INDEXER
//       reconstruction branch specifically (ModuleLoader.cpp's Indexer
//       case is a different arm than the Property case #3618's own T4
//       pinned) — the two could in principle drift independently.
// =========================================================================
BEZEL_TEST(xmod_rule6_indexer_accessor_read_arm, wire_carries_indexer_accessor_purity_3721) {
    constexpr std::string_view kExporterSrc =
        "module IdxLib3721; "
        "public class Bag "
        "  private _n: int := 7; "
        "  public property [i: int]: int "
        "    pure get => i + _n; "
        "  end; "
        "end Bag;";
    auto e = exportModule(kExporterSrc);
    BEZEL_EXPECT_TRUE(e->ok);
    const sema::TypeSymbol* bag = localType(*e, "Bag");
    BEZEL_EXPECT_TRUE(bag != nullptr);

    const sema::PropertySymbol* idx = indexerOf(bag);
    BEZEL_EXPECT_TRUE(idx != nullptr);
    if (idx) BEZEL_EXPECT_TRUE(idx->accessorIsPure);

    bool foundIndexer = false;
    for (const auto& ty : e->meta.types) {
        if (ty.name != "Bag") continue;
        for (const auto& m : ty.members) {
            if (m.kind != MetaMemberKind::Indexer) continue;
            BEZEL_EXPECT_TRUE(m.accessorIsPure);
            foundIndexer = true;
        }
    }
    BEZEL_EXPECT_TRUE(foundIndexer);
}

// =========================================================================
// T5 — the CROSS-MODULE ADMIT: the imported indexer's `pure get` getter
//       admitted by an importer-side `pure` body.
// =========================================================================
BEZEL_TEST(xmod_rule6_indexer_accessor_read_arm, imported_pure_get_indexer_admitted_3721) {
    constexpr std::string_view kExporterSrc =
        "module IdxLib3721B; "
        "public class Bag "
        "  private _n: int := 7; "
        "  public property [i: int]: int "
        "    pure get => i + _n; "
        "  end; "
        "end Bag;";
    constexpr std::string_view kImporterSrc =
        "module App; import IdxLib3721B; "
        "public class Program "
        "  public static pure method Read(b: Bag): int "
        "    return b[3]; "
        "  end Read; "
        "  public static method Main(): int => 0; "
        "end Program;";
    auto e = exportModule(kExporterSrc);
    BEZEL_EXPECT_TRUE(e->ok);
    auto i = importModuleInto(e->meta, kImporterSrc);
    BEZEL_EXPECT_TRUE(i->ok);

    const sema::TypeSymbol* bag = importedType(*i, "Bag");
    BEZEL_EXPECT_TRUE(bag != nullptr);
    const sema::PropertySymbol* idx = indexerOf(bag);
    BEZEL_EXPECT_TRUE(idx != nullptr);
    if (idx) BEZEL_EXPECT_TRUE(idx->accessorIsPure);

    BEZEL_EXPECT_FALSE(rejectsAsImpureAccessorCall(e->meta, kImporterSrc));
}

// =========================================================================
// T6 — the CROSS-MODULE NON-PURE REJECT (control for T5): an imported
//       non-`pure` block-form indexer getter is rejected inside a `pure`
//       body.
// =========================================================================
BEZEL_TEST(xmod_rule6_indexer_accessor_read_arm, imported_non_pure_indexer_rejected_3721) {
    constexpr std::string_view kExporterSrc =
        "module IdxLib3721C; "
        "public class Bag "
        "  public static Calls: int := 0; "
        "  private _n: int := 7; "
        "  public property [i: int]: int "
        "    get => do Calls := Calls + 1; return i + _n; end; "
        "  end; "
        "end Bag;";
    constexpr std::string_view kImporterSrc =
        "module App; import IdxLib3721C; "
        "public class Program "
        "  public static pure method Read(b: Bag): int "
        "    return b[3]; "
        "  end Read; "
        "  public static method Main(): int => 0; "
        "end Program;";
    auto e = exportModule(kExporterSrc);
    BEZEL_EXPECT_TRUE(e->ok);
    auto i = importModuleInto(e->meta, kImporterSrc);
    BEZEL_EXPECT_TRUE(i->ok);

    const sema::TypeSymbol* bag = importedType(*i, "Bag");
    BEZEL_EXPECT_TRUE(bag != nullptr);
    const sema::PropertySymbol* idx = indexerOf(bag);
    BEZEL_EXPECT_TRUE(idx != nullptr);
    if (idx) BEZEL_EXPECT_TRUE(!idx->accessorIsPure);
    if (idx) BEZEL_EXPECT_TRUE(!sema::propertyGetterIsPure(idx));

    BEZEL_EXPECT_TRUE(rejectsAsImpureAccessorCall(e->meta, kImporterSrc));
}

}  // namespace bezel::module
