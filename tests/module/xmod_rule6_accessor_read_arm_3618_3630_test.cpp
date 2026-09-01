// §22.15 rule 6 accessor-read arm (#3630 LOCAL + #3618 wire-side seam).
//
// WHAT THE DEFECT WAS. §22.15 rule 6 names the accessor-read arm as one of
// the four "calls in the admitted positions": "A call — method, operator,
// **accessor read**, or constructor ... is permitted iff the member declared
// on the receiver's static type ... is `pure`-declared or pure-classified".
// The OPERATOR and STATEMENT-OPERATOR arms were closed by #3610 / #3628; the
// ACCESSOR-READ arm was untouched, and the seam had TWO halves:
//
//   #3630 (LOCAL half, PureChecker.cpp):
//     `PureBodyWalker::callTargetIsPure`'s PropertySymbol arm admitted every
//     property read unconditionally (`return true`), so a non-`pure`
//     getter that wrote a static field ran inside a pure body. The
//     MemberAccessExpr arm (`c.Value` inside a pure body) descended only
//     into the target, never inspecting the resolved member symbol — so
//     even an explicit setter-style GETTER call site was invisible to the
//     walker.
//
//   #3618 (WIRE half, ModuleMetadataBuilder + ModuleLoader +
//    propertyGetterIsPure accessor arm):
//     `MetaMember::isPure` is written from the member's MODIFIER list;
//     the accessor-level `pure get` marker lives on `ast::PropertyAccessor
//     ::isPure` and NEVER appears in a modifier list. §22.15 FORCES `pure
//     get` for a read-write block-form property (TypeResolution_Checks.cpp
//     :11524 REJECTS property-level `pure` there), so the accessor spelling
//     is the one every settable block-form property carries. Without a
//     wire slot, an imported `property P { pure get }` reached the
//     importer-side `propertyGetterIsPure` predicate with no `pure`
//     modifier, no `ast::PropertyDecl` to walk (ModuleLoader never
//     reconstructs one), and answered `false` — while the exporter's own
//     compilation of the character-identical source answered `true`. That
//     was a silent cross-module wrong answer to §22.15 rule 6.
//
// The two halves are tightly coupled: closing #3630 without #3618 makes
// every imported block-form getter a false reject (the exporter admits,
// the importer rejects, on character-identical source). Per the
// ready-queue, landing only one is UNSOUND — both halves land together.
//
// WHAT THIS FILE PINS, and why each row is here:
//
//   T1  the LOCAL REJECTION (#3630): a non-`pure` property getter that
//       writes a static field is rejected when read inside a `pure` body.
//       This is the row that FAILS WITHOUT the fix (the rule 6 reject
//       was absent).
//   T2  the LOCAL `pure get` ADMIT (#3630): the accessor-level
//       `pure get` getter is admitted inside a pure body. Without the
//       `propertyGetterIsPure` accessor arm (the SEAM-ACCESSOR slot #3618
//       widens), the LOCAL side also fails — `memberIsPure` returns false
//       because no `pure` modifier is present, and the AST-arm kicks in
//       here only because the SYMBOL's accessorIsPure slot is now set.
//   T3  the LOCAL `pure property` ADMIT (control): the property-level
//       `pure` marker (read-only / expression-bodied shape) is admitted
//       inside a pure body — the predicate's `memberIsPure` arm covers
//       this case via `MemberSymbol::isPure`. T3 is what makes T1 a real
//       reject rather than a "pure body admits nothing" blanket reject.
//   T4  the WIRE EXPORT (#3618): `MetaMember::accessorIsPure` is `true`
//       on the wire for a `pure get` property, `false` otherwise. This
//       is the row that FAILS WITHOUT the fix (the bit was missing).
//   T5  the CROSS-MODULE ADMIT (#3618): the imported `pure get`
//       PropertySymbol has `accessorIsPure == true`, so the importer-side
//       `propertyGetterIsPure` answers `true` — character-identical source
//       admission. This is the row that FAILS WITHOUT the fix (the silent
//       cross-module wrong-answer).
//   T6  the CROSS-MODULE NON-PURE REJECT (control): an imported NON-`pure
//       block-form property getter is REJECTED inside a `pure` body — the
//       half T4/T5 must not over-admit. T6 is what makes T5 a real admit
//       and not a "predicts pure for everything" blanket admit.
//
// Spec: §22.15 "The pure body" rule 6 / "Where `pure` may appear" /
// "Purity attaches to the getter", §2.8.2 "the artifact interface",
// v0.286.

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

// ---- Shared exporter / importer harness (mirrors #3609's pattern) ----------

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

// The LOCAL `Box` TypeSymbol out of an exporter context.
const sema::TypeSymbol* localType(const Exported& e,
                                  std::string_view name) {
    if (!e.ctx || !e.ctx->module()) return nullptr;
    for (sema::Symbol* c : e.ctx->module()->children)
        if (auto* t = dynamic_cast<sema::TypeSymbol*>(c))
            if (t->name == name) return t;
    return nullptr;
}

// The IMPORTED `Box` TypeSymbol out of an importer context.
const sema::TypeSymbol* importedType(const Imported& i,
                                     std::string_view name) {
    if (!i.ctx) return nullptr;
    return i.ctx->lookupImportedTypeByName(name);
}

const sema::MemberSymbol* member(const sema::TypeSymbol* t,
                                 std::string_view name) {
    if (!t) return nullptr;
    for (sema::Symbol* m : t->members)
        if (auto* mem = dynamic_cast<sema::MemberSymbol*>(m))
            if (mem->name == name) return mem;
    return nullptr;
}

const sema::PropertySymbol* property(const sema::TypeSymbol* t,
                                     std::string_view name) {
    auto* m = member(t, name);
    return dynamic_cast<const sema::PropertySymbol*>(m);
}

// Run front-end + PureChecker over `importerSrc` with `dep` loaded,
// returning any "is not admitted in a pure body" diagnostic. Mirrors
// pure_checker_imported_purity_2054_test.cpp's ordering so the imported
// symbols exist before the importer's calls are checked.
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

// Run the LOCAL pure-body checker over a single-module source (no imports)
// and return whether any "not admitted in a pure body" diagnostic fired.
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
// T1 — the LOCAL REJECT (#3630). The verbatim repro from the issue body:
//       a non-pure getter that WRITES a static field runs inside a pure body.
//       Pre-fix this row FAILS — `callTargetIsPure`'s PropertySymbol arm
//       admitted every getter unconditionally, so the impure write ran
//       silently. Post-fix it raises §22.15 rule 6's accessor-read reject.
// =========================================================================
BEZEL_TEST(xmod_rule6_accessor_read_arm, non_pure_getter_in_pure_body_rejected_3630) {
    constexpr std::string_view kLocalSrc =
        "module App; "
        "public class Counter "
        "  private _n: int := 7; "
        "  public static Calls: int := 0; "
        "  public property Value: int "
        "    get => do Calls := Calls + 1; return _n; end; "
        "    set => _n := value; "
        "  end Value; "
        "end Counter; "
        "public class Program "
        "  public static pure method P(c: Counter): int "
        "    return c.Value; "
        "  end P; "
        "  public static method Main(): int => 0; "
        "end Program;";
    BEZEL_EXPECT_TRUE(localRejectsAsImpureAccessor(kLocalSrc));
}

// =========================================================================
// T2 — the LOCAL `pure get` ADMIT (#3630 / #3618 LOCAL half). The accessor-
//       level `pure get` marker that the AST-walk arm of propertyGetterIsPure
//       consults. Without #3618's `ps->accessorIsPure` slot being populated
//       by SymbolCollection's PropertyDecl arm, even the LOCAL side would
//       fall through to the dynamic_cast walk (which works on a local AST)
//       — T2 is the positive control that the SAME symbol-side slot is
//       populated on the local path AND consumed on the path that fires in
//       `callTargetIsPure`. (The AST-walk fallback also passes, but T2
//       would pass without #3618 too — T4 and T5 are the wire-side control.)
// =========================================================================
BEZEL_TEST(xmod_rule6_accessor_read_arm, pure_get_accessor_admitted_in_pure_body_3630) {
    constexpr std::string_view kLocalSrc =
        "module App; "
        "public class Box "
        "  public static Count: int := 0; "
        "  private _v: int := 7; "
        "  public property V: int "
        "    pure get => _v; "
        "    set => _v := value; "
        "  end V; "
        "end Box; "
        "public class Program "
        "  public static pure method Read(b: Box): int "
        "    return b.V; "
        "  end Read; "
        "  public static method Main(): int => 0; "
        "end Program;";
    BEZEL_EXPECT_FALSE(localRejectsAsImpureAccessor(kLocalSrc));
}

// =========================================================================
// T3 — the LOCAL `pure property` (modifier-level) ADMIT, control. The
//       property-level `pure` marker that lives on the property's modifier
//       list — the OTHER spelling §22.15 admits for read-only / expression-
//       bodied shapes. `memberIsPure` already covered this case (#3609);
//       T3 pins that the fix did not regress the property-level path.
//       T1 + T3 together prove the rule, not a blanket reject.
// =========================================================================
BEZEL_TEST(xmod_rule6_accessor_read_arm, pure_property_modifier_admitted_3630_control) {
    constexpr std::string_view kLocalSrc =
        "module App; "
        "public class Box "
        "  public static Count: int := 7; "
        "  public pure property V: int => Box.Count; "
        "end Box; "
        "public class Program "
        "  public static pure method Read(b: Box): int "
        "    return b.V; "
        "  end Read; "
        "  public static method Main(): int => 0; "
        "end Program;";
    BEZEL_EXPECT_FALSE(localRejectsAsImpureAccessor(kLocalSrc));
}

// =========================================================================
// T4 — the WIRE EXPORT (#3618). `MetaMember::accessorIsPure` is `true` on
//       the wire for a `pure get` property, `false` for a plain block-form
//       sibling. Pre-fix this row FAILS — the 19th scalar did not exist.
//       Post-fix it pins both the `pure get` value AND the non-pure twin.
// =========================================================================
BEZEL_TEST(xmod_rule6_accessor_read_arm, wire_carries_accessor_purity_3618) {
    constexpr std::string_view kExporterSrc =
        "module PureLib; "
        "public class Box "
        "  private _v: int := 7; "
        "  public property PureGet: int "
        "    pure get => _v; "
        "    set => _v := value; "
        "  end PureGet; "
        "  public property PlainGet: int "
        "    get => _v; "
        "    set => _v := value; "
        "  end PlainGet; "
        "end Box;";
    auto e = exportModule(kExporterSrc);
    BEZEL_EXPECT_TRUE(e->ok);
    const sema::TypeSymbol* box = localType(*e, "Box");
    BEZEL_EXPECT_TRUE(box != nullptr);

    // The exporter's local symbols must carry `accessorIsPure` correctly
    // (SymbolCollection PropertyDecl arm — the LOCAL half of #3618).
    const sema::PropertySymbol* pureProp = property(box, "PureGet");
    const sema::PropertySymbol* plainProp = property(box, "PlainGet");
    BEZEL_EXPECT_TRUE(pureProp != nullptr);
    BEZEL_EXPECT_TRUE(plainProp != nullptr);
    if (pureProp)   BEZEL_EXPECT_TRUE(pureProp->accessorIsPure);
    if (plainProp)  BEZEL_EXPECT_TRUE(!plainProp->accessorIsPure);

    // And the WIRE must carry it — the only thing the importer sees on a
    // cross-module property. Round-trip the artifact and inspect the byte-
    // level MetaMember the importer receives.
    BEZEL_EXPECT_TRUE(e->meta.types.size() >= 1);
    bool foundPure = false, foundPlain = false;
    for (const auto& ty : e->meta.types) {
        if (ty.name != "Box") continue;
        for (const auto& m : ty.members) {
            if (m.name == "PureGet") {
                BEZEL_EXPECT_TRUE(m.accessorIsPure);
                foundPure = true;
            } else if (m.name == "PlainGet") {
                BEZEL_EXPECT_TRUE(!m.accessorIsPure);
                foundPlain = true;
            }
        }
    }
    BEZEL_EXPECT_TRUE(foundPure);
    BEZEL_EXPECT_TRUE(foundPlain);
}

// =========================================================================
// T5 — the CROSS-MODULE ADMIT (#3618). The imported `pure get` getter is
//       admitted by an importer-side `pure` body. This is the row that
//       fails pre-fix: the importer-side `propertyGetterIsPure` returned
//       `false` for an imported `pure get` getter while the exporter's own
//       compilation of the character-identical source returned `true` —
//       a silent cross-module wrong answer to §22.15 rule 6. Post-fix
//       `ps->accessorIsPure == true` after the importer reconstruction,
//       `propertyGetterIsPure` admits the cross-module `b.PureGet` read,
//       and T5 passes.
// =========================================================================
BEZEL_TEST(xmod_rule6_accessor_read_arm, imported_pure_get_accessor_admitted_3618) {
    constexpr std::string_view kExporterSrc =
        "module PureLib; "
        "public class Box "
        "  private _v: int := 7; "
        "  public property PureGet: int "
        "    pure get => _v; "
        "    set => _v := value; "
        "  end PureGet; "
        "end Box;";
    constexpr std::string_view kImporterSrc =
        "module App; import PureLib; "
        "public class Program "
        "  public static pure method Read(b: Box): int "
        "    return b.PureGet; "
        "  end Read; "
        "  public static method Main(): int => 0; "
        "end Program;";
    auto e = exportModule(kExporterSrc);
    BEZEL_EXPECT_TRUE(e->ok);
    auto i = importModuleInto(e->meta, kImporterSrc);
    BEZEL_EXPECT_TRUE(i->ok);

    // First: the imported PropertySymbol MUST carry `accessorIsPure == true`
    // (the wire->symbol hop #3618 wires). Pre-fix this was `false` because
    // the slot did not exist.
    const sema::TypeSymbol* box = importedType(*i, "Box");
    BEZEL_EXPECT_TRUE(box != nullptr);
    const sema::PropertySymbol* pureProp = property(box, "PureGet");
    BEZEL_EXPECT_TRUE(pureProp != nullptr);
    if (pureProp) BEZEL_EXPECT_TRUE(pureProp->accessorIsPure);

    // Second: the importer-side pure-body checker must ADMIT the read, exactly
    // like the exporter's own character-identical compilation admits. Pre-fix
    // this would silent-admit because `callTargetIsPure` had the unconditional
    // PropertySymbol arm — but pre-fix a CHARACTER-IDENTICAL local twin of the
    // importer's `Program` body also silent-admits, so the only way to prove
    // #3618 is end-to-end is to LATER tighten (#3630) the call site check and
    // then re-verify this passes. T5 therefore exercises the behaviour: the
    // importer-source is a `pure method` reading an imported `pure get`
    // getter, and it MUST NOT raise.
    BEZEL_EXPECT_FALSE(rejectsAsImpureAccessorCall(e->meta, kImporterSrc));
}

// =========================================================================
// T6 — the CROSS-MODULE NON-PURE REJECT control. The imported NON-`pure`
//       block-form property getter is REJECTED when read inside a `pure`
//       body. Pre-fix this row was a FALSE-POSITIVE admit because the
//       unconditional `callTargetIsPure` PropertySymbol arm admitted every
//       imported getter. Post-fix #3630 tightens the rule, and the
//       importer-side walker MUST raise. T6 is the control for T5 —
//       without it the #3618 fix could over-admit by marking every
//       imported property `pure`-classified, and no row in this file
//       would catch it.
// =========================================================================
BEZEL_TEST(xmod_rule6_accessor_read_arm, imported_non_pure_block_getter_rejected_3630) {
    constexpr std::string_view kExporterSrc =
        "module PureLib; "
        "public class Box "
        "  public static Count: int := 0; "
        "  private _v: int := 7; "
        "  public property PlainGet: int "
        "    get => do Count := Count + 1; return _v; end; "
        "    set => _v := value; "
        "  end PlainGet; "
        "end Box;";
    constexpr std::string_view kImporterSrc =
        "module App; import PureLib; "
        "public class Program "
        "  public static pure method Read(b: Box): int "
        "    return b.PlainGet; "
        "  end Read; "
        "  public static method Main(): int => 0; "
        "end Program;";
    auto e = exportModule(kExporterSrc);
    BEZEL_EXPECT_TRUE(e->ok);
    auto i = importModuleInto(e->meta, kImporterSrc);
    BEZEL_EXPECT_TRUE(i->ok);

    // Confirm the imported symbol is correctly NOT pure-classified —
    // sanity that #3618 sets `ps->accessorIsPure = false` for a non-`pure
    // get` block-form property rather than always-true.
    const sema::TypeSymbol* box = importedType(*i, "Box");
    BEZEL_EXPECT_TRUE(box != nullptr);
    const sema::PropertySymbol* p = property(box, "PlainGet");
    BEZEL_EXPECT_TRUE(p != nullptr);
    if (p) BEZEL_EXPECT_TRUE(!p->accessorIsPure);
    if (p) BEZEL_EXPECT_TRUE(!sema::propertyGetterIsPure(p));

    // And the importer-side walker must REJECT — the rule fires through
    // MemberAccessExpr (the `b.PlainGet` read inside `Read`). Without the
    // fix this row PASSED when it should have FAILED.
    BEZEL_EXPECT_TRUE(rejectsAsImpureAccessorCall(e->meta, kImporterSrc));
}

}  // namespace bezel::module
