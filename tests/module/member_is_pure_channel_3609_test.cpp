// fix #3609 (§22.15 / §2.8.2, v0.286): `MemberSymbol::isPure` is a BOTH-SOURCES
// channel, and one predicate answers "is this member `pure`?".
//
// WHAT THE DEFECT WAS. `MemberSymbol::isPure` was written only by
// `ModuleLoader` (`:735` in `applyBehavior`, `:4311` on the imported-extension
// path); `SymbolCollection` never set it, so for every LOCAL member the field
// read `false` and the field meant "imported AND pure" rather than "pure". Five
// sites in `PureChecker.cpp` each re-derived the question from a DIFFERENT
// subset of the available sources (symbol field / `ast::OperatorDecl::isPure` /
// `ast::ConstructorDecl::isPure` / the `pure` modifier / the accessor-level
// `pure get`), which is the duplicated-classification-predicate shape that
// generated #3174 in this very rule.
//
// ⚠ IT WAS NOT PRODUCING A WRONG ANSWER, and saying so is the point. `fix
// #2054` added the field and, in the same commit, made `applyBehavior` FORGE a
// `pure` entry in the imported member's `modifiers` — its commit body says so:
// "pushes \"pure\" into the modifiers list so existing modifier sweeps still
// see it". Every consumer scans `modifiers`, so the field has been redundant
// since birth and four cross-module probes on `af11a5d` answered correctly. The
// fix therefore changes NO admitted program; what it changes is that the field
// now means the same thing on both sides of the artifact boundary, which is
// what makes routing the five copies onto one predicate safe.
//
// WHAT THIS FILE PINS, and why each row is here:
//
//   T1  the LOCAL half of the channel — `MemberSymbol::isPure` is true for a
//       `pure` method, operator, constructor and property in a locally-compiled
//       module. This is the row that FAILS WITHOUT the fix (four false-vs-true
//       assertions); everything else in the file passes on both sides.
//   T2  the channel MEANS THE SAME on both sides: for the character-identical
//       source, compiled here and then round-tripped through
//       `buildModuleMetadata` -> `loadMetadata`, the local symbol and the
//       imported symbol agree on `isPure` AND on `memberIsPure`, member by
//       member. Pre-fix the local side reads `false` where the imported side
//       reads `true`, so this row fails too.
//   T3  THE OVER-REJECTION CONTROL. A predicate that answered "pure" to
//       everything would pass T1 and T2. T3 pins that the NON-pure twins —
//       character-identical but for the marker — read false on BOTH sides.
//   T4  the §22.15 VERDICT is unchanged in both directions across the boundary:
//       a pure body calling the imported IMPURE method is still rejected, and
//       calling the imported PURE method is still admitted. This is the
//       behaviour control for the routing of `callTargetIsPure`.
//   T5  `propertyGetterIsPure` keeps the accessor-level arm that
//       `memberIsPure` does not have. Routing `checkOneMember`'s property arm
//       through the member-level predicate alone would have stopped walking a
//       `pure get` body — a loud->quiet inversion no other row would catch.
//
// Spec: §22.15 "The pure body" rule 6 / "Where `pure` may appear" / "Pure
// constructors", §2.8.2 "the artifact interface", v0.286.

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

// The ONE source text every row uses. `Calc` carries a `pure` and a non-pure
// twin of each member form `pure` is admitted on (§22.15 "Where `pure` may
// appear"): method, operator, constructor, property. The twins differ ONLY in
// the marker, which is what makes T3 a control rather than a second positive.
//
// Notes on the shapes, each forced by a rule rather than chosen:
//   * the operators are `public static method operator <tok>` (A.4
//     OperatorDecl) and carry `pure` in the modifier prefix — the parser folds
//     it into `ast::OperatorDecl::isPure` and does NOT leave it in a modifier
//     list, so a local operator's purity has exactly one source;
//   * the properties are EXPRESSION-BODIED. §22.15 rejects property-level
//     `pure` on the auto form (redundant) and on a read-write block form
//     (per-accessor `pure get` instead), so the expression-bodied shape is the
//     one that carries a property-level marker legally;
//   * `PureRead`/`PlainRead` return `int` and read a `static readonly` field,
//     so the pure one's body satisfies rule 1 and the class needs no state.
constexpr std::string_view kExporterSrc =
    "module PureLib; "
    "public class Calc "
    "  public static readonly Seed: int := 7; "
    "  public pure constructor() end constructor; "
    "  public static pure method PureAdd(a: int, b: int): int => a + b; "
    "  public static method PlainAdd(a: int, b: int): int => a + b; "
    "  public static pure method operator +(x: Calc, y: Calc): int => 1; "
    "  public static method operator -(x: Calc, y: Calc): int => 2; "
    "  public pure property PureRead: int => Calc.Seed; "
    "  public property PlainRead: int => Calc.Seed; "
    "end Calc;";

// Run the driver's front-end ordering on `source` and build the artifact
// interface from the resulting symbol table — the same two steps a real `-c`
// compile performs (Driver.cpp), so the metadata under test is the metadata a
// real exporter would write.
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

// Load `dep` into a fresh importing context. The importer's own source is a
// bare module so nothing but the reconstruction under test is in the table.
struct Imported {
    DiagnosticEngine diagnostics;
    std::unique_ptr<ast::SourceFile> sourceFile;
    std::unique_ptr<sema::SemaContext> ctx;
    bool ok = false;
};

std::unique_ptr<Imported> importModule(const ModuleMetadata& dep) {
    auto r = std::make_unique<Imported>();
    static constexpr std::string_view kImporterSrc =
        "module App; import PureLib; "
        "public class Program "
        "  public static method Main(): int => 0; "
        "end Program;";
    Lexer lexer(kImporterSrc, r->diagnostics);
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

// The LOCAL `Calc` TypeSymbol out of an exporter context.
const sema::TypeSymbol* localCalc(const Exported& e) {
    if (!e.ctx || !e.ctx->module()) return nullptr;
    for (sema::Symbol* c : e.ctx->module()->children)
        if (auto* t = dynamic_cast<sema::TypeSymbol*>(c))
            if (t->name == "Calc") return t;
    return nullptr;
}

// The IMPORTED `Calc` TypeSymbol out of an importer context. The reconstructed
// type is not a child of the importer's own module, so it is reached through
// the context's imported-type registry rather than by walking `children`.
const sema::TypeSymbol* importedCalc(const Imported& i) {
    if (!i.ctx) return nullptr;
    return i.ctx->lookupImportedTypeByName("Calc");
}

// First member of `t` whose name matches. Constructors are keyed `.ctor` and
// operators by their token (SymbolCollection.cpp / ModuleLoader.cpp both).
const sema::MemberSymbol* member(const sema::TypeSymbol* t,
                                 std::string_view name) {
    if (!t) return nullptr;
    for (sema::Symbol* m : t->members)
        if (auto* mem = dynamic_cast<sema::MemberSymbol*>(m))
            if (mem->name == name) return mem;
    return nullptr;
}

// Every member this file asserts on, with the answer §22.15 requires. Kept as
// one table so T1/T2/T3 cannot drift apart: T1 reads the local side, T3 reads
// the `false` rows on both sides, and T2 compares the two sides row by row.
struct Row {
    const char* name;
    bool pure;
};
constexpr Row kRows[] = {
    {".ctor",     true},
    {"PureAdd",   true},
    {"PlainAdd",  false},
    {"+",         true},
    {"-",         false},
    {"PureRead",  true},
    {"PlainRead", false},
};

}  // namespace

// =========================================================================
// T1 — the LOCAL half of the channel. THE ROW THAT FAILS WITHOUT THE FIX.
// =========================================================================
BEZEL_TEST(member_is_pure_channel_3609,
           local_member_symbol_carries_declared_purity_spec_22_15) {
    auto e = exportModule(kExporterSrc);
    BEZEL_EXPECT_TRUE(e->ok);
    const sema::TypeSymbol* calc = localCalc(*e);
    BEZEL_EXPECT_TRUE(calc != nullptr);
    for (const Row& row : kRows) {
        const sema::MemberSymbol* m = member(calc, row.name);
        BEZEL_EXPECT_TRUE(m != nullptr);
        if (!m) continue;
        // The FIELD, not the predicate: pre-#3609 `SymbolCollection` never
        // assigned it, so all four `true` rows read `false` here.
        BEZEL_EXPECT_EQ(m->isPure, row.pure);
        // And the predicate agrees with the field on the local side.
        BEZEL_EXPECT_EQ(sema::memberIsPure(m), row.pure);
    }
}

// =========================================================================
// T2 — the channel means the SAME THING on both sides of §2.8.2's boundary.
// Character-identical source: the local symbols and the symbols reconstructed
// from this exact module's own artifact interface must agree member by member.
// =========================================================================
BEZEL_TEST(member_is_pure_channel_3609,
           imported_and_local_twin_agree_on_purity_spec_2_8_2) {
    auto e = exportModule(kExporterSrc);
    BEZEL_EXPECT_TRUE(e->ok);
    auto i = importModule(e->meta);
    BEZEL_EXPECT_TRUE(i->ok);
    const sema::TypeSymbol* localT = localCalc(*e);
    const sema::TypeSymbol* impT = importedCalc(*i);
    BEZEL_EXPECT_TRUE(localT != nullptr);
    BEZEL_EXPECT_TRUE(impT != nullptr);
    for (const Row& row : kRows) {
        const sema::MemberSymbol* lm = member(localT, row.name);
        const sema::MemberSymbol* im = member(impT, row.name);
        BEZEL_EXPECT_TRUE(lm != nullptr);
        BEZEL_EXPECT_TRUE(im != nullptr);
        if (!lm || !im) continue;
        BEZEL_EXPECT_EQ(im->isPure, row.pure);
        BEZEL_EXPECT_EQ(lm->isPure, im->isPure);
        BEZEL_EXPECT_EQ(sema::memberIsPure(lm), sema::memberIsPure(im));
    }
}

// =========================================================================
// T3 — THE OVER-REJECTION CONTROL. A predicate that said "pure" to everything
// would satisfy T1 and T2; these are the rows that make it a real test set.
// The non-pure twins are character-identical to the pure ones but for the
// marker, so a false "yes" here can only come from the predicate.
// =========================================================================
BEZEL_TEST(member_is_pure_channel_3609,
           unmarked_twins_are_not_pure_on_either_side_spec_22_15) {
    auto e = exportModule(kExporterSrc);
    auto i = importModule(e->meta);
    BEZEL_EXPECT_TRUE(e->ok);
    BEZEL_EXPECT_TRUE(i->ok);
    const sema::TypeSymbol* localT = localCalc(*e);
    const sema::TypeSymbol* impT = importedCalc(*i);
    BEZEL_EXPECT_TRUE(localT != nullptr);
    BEZEL_EXPECT_TRUE(impT != nullptr);
    for (const char* name : {"PlainAdd", "-", "PlainRead"}) {
        const sema::MemberSymbol* lm = member(localT, name);
        const sema::MemberSymbol* im = member(impT, name);
        BEZEL_EXPECT_TRUE(lm != nullptr);
        BEZEL_EXPECT_TRUE(im != nullptr);
        if (!lm || !im) continue;
        BEZEL_EXPECT_TRUE(!sema::memberIsPure(lm));
        BEZEL_EXPECT_TRUE(!sema::memberIsPure(im));
        BEZEL_EXPECT_TRUE(!lm->isPure);
        BEZEL_EXPECT_TRUE(!im->isPure);
    }
}

namespace {

// Run the front end + PureChecker over `importerSrc` with `dep` loaded, and
// report whether §22.15's calls-to-pure-members rule fired. Mirrors
// pure_checker_imported_purity_2054_test.cpp's ordering so the imported
// symbols exist before the importer's calls are checked.
bool rejectsAsImpureCall(const ModuleMetadata& dep,
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

}  // namespace

// =========================================================================
// T4 — the BEHAVIOUR control for routing `callTargetIsPure` through the one
// predicate: §22.15 rule 6's verdict is unchanged in BOTH directions across
// the boundary. Rejecting the impure call alone would also pass on a compiler
// that rejected every imported call, so the admitted half is half the row.
// =========================================================================
BEZEL_TEST(member_is_pure_channel_3609,
           imported_call_purity_verdict_unchanged_spec_22_15) {
    auto e = exportModule(kExporterSrc);
    BEZEL_EXPECT_TRUE(e->ok);
    BEZEL_EXPECT_TRUE(rejectsAsImpureCall(
        e->meta,
        "module App; import PureLib; "
        "public class Program "
        "  public static pure method P(): int => Calc.PlainAdd(1, 2); "
        "  public static method Main(): int => 0; "
        "end Program;"));
    BEZEL_EXPECT_TRUE(!rejectsAsImpureCall(
        e->meta,
        "module App; import PureLib; "
        "public class Program "
        "  public static pure method P(): int => Calc.PureAdd(1, 2); "
        "  public static method Main(): int => 0; "
        "end Program;"));
}

// =========================================================================
// T5 — `propertyGetterIsPure` is WIDER than `memberIsPure`, and must stay so.
// §22.15 attaches purity to the getter, and the accessor-level `pure get`
// marker rides `ast::PropertyAccessor::isPure` — a channel neither the
// property's modifier list nor `MetaMember` carries. `checkOneMember`'s
// property arm is what walks a `pure get` body; routing it through the
// member-level predicate would have silently stopped walking it, turning a
// §22.15 diagnostic into nothing. This row is what forbids that.
// =========================================================================
BEZEL_TEST(member_is_pure_channel_3609,
           accessor_level_pure_get_is_only_seen_by_the_getter_predicate_spec_22_15) {
    auto e = exportModule(
        "module GetLib; "
        "public class Box "
        "  public static readonly Seed: int := 3; "
        "  public property Peek: int "
        "    pure get => Box.Seed; "
        "  end Peek; "
        "end Box;");
    BEZEL_EXPECT_TRUE(e->ok);
    const sema::TypeSymbol* box = nullptr;
    for (sema::Symbol* c : e->ctx->module()->children)
        if (auto* t = dynamic_cast<sema::TypeSymbol*>(c))
            if (t->name == "Box") box = t;
    BEZEL_EXPECT_TRUE(box != nullptr);
    const auto* ps =
        dynamic_cast<const sema::PropertySymbol*>(member(box, "Peek"));
    BEZEL_EXPECT_TRUE(ps != nullptr);
    if (!ps) return;
    // The property itself carries no `pure` modifier and no symbol-side bit —
    // the marker is on the accessor.
    BEZEL_EXPECT_TRUE(!sema::memberIsPure(ps));
    BEZEL_EXPECT_TRUE(sema::propertyGetterIsPure(ps));
}

}  // namespace bezel::module
