// §22.15 / §2.8.2 (v0.204, #2054): PureChecker must admit a call to a method
// OR a `new` to a constructor whose purity was CARRIED across an artifact
// boundary. Before this fix, an imported member's `isPure` field was unset
// (the loader never propagated the carried purity bit), so:
//   - an imported MethodSymbol was rejected at the call site in a pure body
//     (`callTargetIsPure` returned false because `ms->decl == nullptr`
//     suppressed the AST path and the modifiers list did not carry "pure");
//   - an imported ConstructorSymbol was rejected at the throw carve-out
//     (`throwCarveOutAdmits` returned false for the same reason, breaking
//     `throw new ImportedClass(args)` in a pure body).
//
// The fix's two halves:
//   1. ModuleLoader::convertMember's `applyBehavior` (line 305 in
//      src/module/ModuleLoader.cpp) sets `member->isPure = mm.isPure` AND
//      pushes "pure" into the member's `modifiers` so existing modifier
//      sweeps still see it.
//   2. PureChecker::callTargetIsPure (src/sema/PureChecker.cpp) gains an
//      EXPLICIT `if (ms->isPure) return true;` (and matching
//      `if (cs->isPure) return true;` for ConstructorSymbol) — the surgical
//      modifier-sweep fix that pins the admitted field at the call site.
//
// This file pins BOTH halves via the public `loadMetadata` entry point: it
// builds the dependency metadata by hand (so the test controls every
// contract-facts field directly), loads it as an importer-side symbol, and
// then has a pure body exercise the imported member. Without the fix, the
// body emits a §22.15 purity violation; with the fix, it is admitted.

#include "module/ModuleLoader.hpp"
#include "module/ModuleMetadata.hpp"

#include <memory>
#include <string>

#include "ast/Node.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/PureChecker.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

// Local helper: a primitive type leaf (§4). Mirrors the one in
// module_metadata_test.cpp (which lives in an anonymous namespace and so is
// not linkable from this translation unit).
MetaType prim(PrimitiveKind p, bool nullable = false) {
    MetaType t;
    t.kind = TypeKind::Primitive;
    t.primitive = p;
    t.nullable = nullable;
    return t;
}

// Build a hand-crafted ModuleMetadata for module `MathLib` with:
//   - class Calc
//   - public static pure method Square(n: int): int  (isPure=true on the member)
//   - public pure constructor Calc()                  (isPure=true on the member)
//   - no destructor, no owned fields (trivial-dispose — required for the
//     throw carve-out's second admission gate, §22.15 rule 6(b)).
ModuleMetadata buildMathLibWithImportedPurity() {
    ModuleMetadata md;
    md.moduleName = "MathLib";
    md.exports = {"Calc", "Impure"};
    md.contractRecordsCarried = true;

    MetaTypeDecl calc;
    calc.name = "Calc";
    calc.kind = MetaTypeDeclKind::Class;
    calc.visibility = MetaVisibility::Public;

    // §31.1 (v0.243, #3046): every throw operand must be an `Exception`-derived
    // class. `imported_pure_constructor_throw_admitted_spec_22_15` below throws
    // `new MathLib.Calc()`, so the carried class must derive from `Exception`
    // or the §31.1 operand check rejects the program before §22.15's carve-out
    // is ever reached. This fixture predates v0.243 and carried neither field.
    //
    // Carried exactly as a real exporter carries it, which is BOTH:
    //   - `builtinExceptionBase`, the dedicated field ModuleMetadataBuilder
    //     copies off the exporter's TypeSymbol (set by TypeResolution_Names for
    //     a class whose base names a built-in exception) and ModuleLoader
    //     restores — this is the field `sema::builtinExceptionBaseOf` reads
    //     first, and the one that makes the §31.1 walk succeed across the
    //     module boundary without an `ast::TypeDecl` (#2950);
    //   - the `baseTypes` entry, which is what the §12 base-class convention
    //     records (baseTypes[0] is the base class).
    calc.builtinExceptionBase = "Exception";
    {
        MetaType exc;
        exc.kind = TypeKind::Named;
        exc.namedName = "Exception";
        calc.baseTypes.push_back(exc);
    }

    // §22.15 / #2054: pure constructor carried across the boundary.
    {
        MetaMember ctor;
        ctor.kind = MetaMemberKind::Constructor;
        ctor.name = "Calc";
        ctor.visibility = MetaVisibility::Public;
        ctor.isPure = true;  // <-- the carried purity under test
        calc.members.push_back(ctor);
    }

    // §22.15 / #2054: pure method carried across the boundary.
    {
        MetaMember square;
        square.kind = MetaMemberKind::Method;
        square.name = "Square";
        square.visibility = MetaVisibility::Public;
        square.isPure = true;  // <-- the carried purity under test
        MetaParam n;
        n.name = "n";
        n.type = prim(PrimitiveKind::Int);
        square.params.push_back(n);
        square.type = prim(PrimitiveKind::Int);
        calc.members.push_back(square);
    }

    md.types.push_back(calc);

    // The CONSTRUCTOR-side negative control (see the test of the same name).
    // Identical to `Calc` in every respect the throw carve-out reads EXCEPT
    // that its constructor is carried NOT pure: same class kind, same
    // Exception base, no destructor and no owned fields (so still
    // trivial-dispose, gate (b) of §22.15's carve-out). The only difference
    // between admitting `new MathLib.Calc()` and rejecting
    // `new MathLib.Impure()` is therefore the carried `isPure` bit itself.
    MetaTypeDecl impure;
    impure.name = "Impure";
    impure.kind = MetaTypeDeclKind::Class;
    impure.visibility = MetaVisibility::Public;
    impure.builtinExceptionBase = "Exception";  // §31.1 (v0.243): throwable
    {
        MetaType exc;
        exc.kind = TypeKind::Named;
        exc.namedName = "Exception";
        impure.baseTypes.push_back(exc);
    }
    {
        MetaMember ctor;
        ctor.kind = MetaMemberKind::Constructor;
        ctor.name = "Impure";
        ctor.visibility = MetaVisibility::Public;
        ctor.isPure = false;  // <-- the whole point of this class
        impure.members.push_back(ctor);
    }
    md.types.push_back(impure);
    return md;
}

// Run parse + symbol-collection + type-resolution + expr-typecheck +
// PureChecker on `src` after loading `dep`'s metadata into the importer's
// SemaContext. Mirrors the module_chapter_1365 harness ordering so the
// imported symbols are reconstructed (pass 1+2 in loadMetadata) before any
// importer-side calls are type-checked.
struct PureCheckResult {
    DiagnosticEngine diagnostics;
    std::unique_ptr<ast::SourceFile> sourceFile;
    std::unique_ptr<sema::SemaContext> ctx;
    bool hasPurityError = false;
};

std::unique_ptr<PureCheckResult> runWithImportedMathLib(
    const ModuleMetadata& dep, std::string_view importerSrc) {
    auto r = std::make_unique<PureCheckResult>();
    Lexer lexer(importerSrc, r->diagnostics);
    Parser parser(lexer, r->diagnostics);
    r->sourceFile = parser.parseCompilationUnit();
    if (!r->sourceFile) return r;
    r->ctx = std::make_unique<sema::SemaContext>(r->diagnostics);
    sema::collectSymbols(*r->sourceFile, *r->ctx);
    // §2054 S0: loadMetadata publishes the carried purity onto the
    // reconstructed MethodSymbol/ConstructorSymbol.
    loadMetadata(dep, *r->ctx);
    sema::resolveTypes(*r->sourceFile, *r->ctx);
    sema::typecheckExpressions(*r->sourceFile, *r->ctx);
    sema::checkPureBodies(*r->sourceFile, *r->ctx);

    for (const auto& d : r->diagnostics.diagnostics()) {
        if (d.severity == Severity::Error &&
            d.message.find("not admitted in a pure body") != std::string::npos) {
            r->hasPurityError = true;
        }
    }
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// #2054 S0 / spec §22.15: a call to an imported pure MethodSymbol from a pure
// body must be admitted. The pure body's typecheck looks at the call target
// `MathLib.Calc.Square` — an imported MethodSymbol reconstructed by
// loadMetadata; the admission gate is `callTargetIsPure`.
//
// Before fix: the loader did not propagate carried purity (`ms->isPure`
// stayed false, modifiers didn't include "pure"); PureChecker rejected with
// "the resolved target `Square` is not pure-declared".
// After fix: `ms->isPure` is true (loadMetadata's applyBehavior) and
// PureChecker's explicit `if (ms->isPure) return true;` admits the call.
// ---------------------------------------------------------------------------
BEZEL_TEST(pure_checker_imported_purity_2054,
           imported_pure_method_admitted_spec_22_15) {
    ModuleMetadata dep = buildMathLibWithImportedPurity();
    auto r = runWithImportedMathLib(
        dep,
        "module App; "
        "import MathLib; "
        "public class Program "
        "  public pure method Double(n: int): int "
        "    return MathLib.Calc.Square(n) + MathLib.Calc.Square(n); "
        "  end Double; "
        "  public static method Main(): int => 0; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r->diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r->hasPurityError);
}

// ---------------------------------------------------------------------------
// #2054 S0 / spec §22.15: `throw new ImportedClass(args)` in a pure body is
// admitted by the throw carve-out IF the invoked constructor is pure AND the
// class is trivial-dispose. The constructor's purity rides on the
// ConstructorSymbol reconstructed by loadMetadata — the test below forces
// the path through `throwCarveOutAdmits` which looks at `cs->isPure`
// directly (the surgical modifier-sweep fix in PureChecker).
//
// Before fix: loader did not set `cs->isPure`; `throwCarveOutAdmits` fell
// through every admission gate and rejected with "the throw carve-out
// admits only `new E(...)` with a pure constructor of a trivial-dispose
// class".
// After fix: `cs->isPure` is true (loadMetadata); the explicit
// `if (cs->isPure) return true;` short-circuits throwCarveOutAdmits.
// ---------------------------------------------------------------------------
BEZEL_TEST(pure_checker_imported_purity_2054,
           imported_pure_constructor_throw_admitted_spec_22_15) {
    ModuleMetadata dep = buildMathLibWithImportedPurity();
    auto r = runWithImportedMathLib(
        dep,
        "module App; "
        "import MathLib; "
        "public class Program "
        "  public pure method Checked(low: int, n: int): int "
        "    if n < low then "
        "      throw new MathLib.Calc(); "
        "    end if; "
        "    return n; "
        "  end Checked; "
        "  public static method Main(): int => 0; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r->diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r->hasPurityError);
}

// ---------------------------------------------------------------------------
// Control: a method NOT marked pure in the carried metadata is still
// REJECTED in a pure body. This pins the boundary — the imported-pure
// admission must not silently admit non-pure carried members (a regression
// to "always admit" would break this control).
// ---------------------------------------------------------------------------
BEZEL_TEST(pure_checker_imported_purity_2054,
           imported_non_pure_method_rejected_spec_22_15) {
    ModuleMetadata dep = buildMathLibWithImportedPurity();
    // Demote `Square` to non-pure by rebuilding metadata directly so the
    // module_chapter_1365 parser isn't a confounding path.
    ModuleMetadata notPure = dep;
    for (auto& t : notPure.types) {
        for (auto& m : t.members) {
            if (m.kind == MetaMemberKind::Method && m.name == "Square") {
                m.isPure = false;
            }
        }
    }
    // Strip the "pure" modifier the loader would add — we don't actually
    // load this metadata, but for fidelity the control case must look like
    // a real non-pure carried method.
    auto r = runWithImportedMathLib(
        notPure,
        "module App; "
        "import MathLib; "
        "public class Program "
        "  public pure method Double(n: int): int "
        "    return MathLib.Calc.Square(n) + MathLib.Calc.Square(n); "
        "  end Double; "
        "  public static method Main(): int => 0; "
        "end Program;");
    // The pure call site must produce a §22.15 purity diagnostic.
    BEZEL_EXPECT_TRUE(r->hasPurityError);
}


// ---------------------------------------------------------------------------
// CONSTRUCTOR-side control for `imported_pure_constructor_throw_admitted`.
//
// WHY THIS EXISTS. That test asserts an ABSENCE (`!hasErrors()`), and
// `PureBodyWalker::throwCarveOutAdmits` (src/sema/PureChecker.cpp:425) opens
// with:
//
//     if (!rt || rt->kind != TypeRef::Kind::Named || !rt->declaration)
//         return true;   // "conservatively admit unknown new-targets"
//
// So a fixture whose class fails to RESOLVE is admitted by that early return
// and produces exactly the same green as one whose carried `isPure` was
// loaded correctly. The admitted-test alone therefore cannot distinguish
// "the carried purity bit works" -- the thing it exists to pin -- from
// "the imported symbol was never reconstructed at all".
//
// This control closes that hole permanently. `Impure` differs from `Calc`
// ONLY in its carried `isPure`, so:
//   - if symbol reconstruction breaks, `rt->declaration` goes null, the early
//     return admits, and THIS test fails loudly;
//   - if the loader stops publishing `isPure` onto the reconstructed
//     ConstructorSymbol, the admitted-test fails.
// The pair brackets the behaviour from both sides; neither alone does.
//
// The method side already had its control (`imported_non_pure_method_
// rejected_spec_22_15`); the constructor side did not. That asymmetry is what
// let the gap sit unnoticed.
// ---------------------------------------------------------------------------
BEZEL_TEST(pure_checker_imported_purity_2054,
           imported_non_pure_constructor_throw_rejected_spec_22_15) {
    ModuleMetadata dep = buildMathLibWithImportedPurity();
    auto r = runWithImportedMathLib(
        dep,
        "module App; "
        "import MathLib; "
        "public class Program "
        "  public pure method Checked(low: int, n: int): int "
        "    if n < low then "
        "      throw new MathLib.Impure(); "
        "    end if; "
        "    return n; "
        "  end Checked; "
        "  public static method Main(): int => 0; "
        "end Program;");
    // §22.15 carve-out gate (a): the invoked constructor is NOT pure, so the
    // throw is not admitted in a pure body.
    BEZEL_EXPECT_TRUE(r->hasPurityError);
}

} // namespace bezel::module