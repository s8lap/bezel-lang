// fix #3223 (§22.15 / §28.9 / §2.8.2, v0.254): an IMPORTED
// non-trivial-dispose class is still ADMITTED as a pure-body construction by
// PureChecker — the FOURTH independent copy of §28.9's classification
// (after `common::isNonTrivialDisposeClass`, `Ownership.cpp`'s §28.3 auto-drop
// gate, and `ExprTypecheck.cpp`'s §21.9 site).
//
// SISTER ISSUE TO #3191. #3191's lane measured three sites (the shared copy +
// the §21.9 walk + the §28.3 gate) and shipped them (commit `43d6abc`).
// PureChecker's walker was NOT in scope for #3191's verify-first — it doesn't
// appear in that issue's measured shapes, and the lane's budget ran out before
// a third classification site could be characterized. Surfacing it here keeps
// #3191's diff clean and makes the residual trackable.
//
// WHAT THE DEFECT IS. `PureBodyWalker::isTrivialDisposeClass`
// (src/sema/PureChecker.cpp:141) is a local copy of §28.9's classification
// that walks `TypeSymbol::members` for a `DestructorSymbol` and an `owned`
// field, per level. `ModuleLoader` builds a `DestructorSymbol` on exactly one
// path (the generic body re-parse), so for a NON-GENERIC imported class the
// walk finds nothing and the predicate answers "trivial-dispose" — same
// defect class as #3191, same shape as the three sites #3191 fixed.
//
// The fix is the SAME consultation #3191's shared copy performs: at each
// level of the walk, read `cur->importedNonTrivialDispose`. Per level, not
// once at the head, so a LOCAL class deriving from an imported non-trivial
// base is decided when the walk REACHES that base (matches the shared copy's
// rule in `OwnershipFacts.cpp:71` and the §21.9 walk's rule in
// `ExprTypecheck.cpp:11258`). A `false` verdict never short-circuits —
// absent (a pre-#3191 artifact) and genuinely-trivial read alike, both fall
// through to the member scan.
//
// Reachable with NO generics and NO `owned` marker anywhere.
//
// MEASURED PRE-FIX on `226aca1` (loadMetadata → PureChecker, sema-only):
//
//   shape                                          single-module twin
//   ──────────────────────────────────────────    ─────────────────────
//   throw new Res(args) in a pure body,            REJECTED, §22.15
//   Res is IMPORTED with `nonTrivialDispose=true`   ("class construction
//                                                    at `throw`")
//
//   shape                                          cross-module
//   ──────────────────────────────────────────    ─────────────────────
//   same as above                                  COMPILED CLEAN
//                                                   (no diagnostic — false
//                                                   accept, destructor never
//                                                   consulted)
//
// ⚠ THIS FIX MOVES IN THE REJECT DIRECTION, SO THE CONTROLS ARE HALF THE TEST.
// A set that only pinned what must be rejected would also pass on a compiler
// that rejected every imported-throw in a pure body — which would over-fire
// for trivial-dispose imports. T3 pins the OVER-REJECTION CONTROL: an imported
// TRIVIAL-dispose class must still be admitted by the throw carve-out.
// T4 pins the SAME-MODULE TWIN of T1: §22.15 must agree across the boundary,
// which is what makes T1 a CROSS-MODULE claim rather than a §22.15 claim.

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

// Build a hand-crafted ModuleMetadata for module `DtorLib` with a single
// CLASS that carries `nonTrivialDispose = true` and a pure constructor
// `ResDtor()` — the throw carve-out needs the SECOND gate (`cs->isPure`) to
// pass; the FIRST gate (`isTrivialDisposeClass`) is what this fix lands on.
// Derives from `Exception` so `throw new ResDtor()` is admissible under §31.1.
ModuleMetadata buildDtorLib() {
    ModuleMetadata md;
    md.moduleName = "DtorLib";
    md.exports = {"ResDtor"};
    md.contractRecordsCarried = true;

    MetaTypeDecl resDtor;
    resDtor.name = "ResDtor";
    resDtor.kind = MetaTypeDeclKind::Class;
    resDtor.visibility = MetaVisibility::Public;
    // §2.8.2 / fix #3191: the §3.6 trivial-dispose verdict, INVERTED on the
    // wire as `nonTrivialDispose`. This is the verdict ModuleLoader carries
    // onto `TypeSymbol::importedNonTrivialDispose` — which the fix's local
    // `isTrivialDisposeClass` now consults per level.
    resDtor.nonTrivialDispose = true;
    // §31.1: throw operand must derive from `Exception` — without this the
    // §31.1 operand check rejects before §22.15's carve-out is reached.
    resDtor.builtinExceptionBase = "Exception";
    {
        MetaType exc;
        exc.kind = TypeKind::Named;
        exc.namedName = "Exception";
        resDtor.baseTypes.push_back(exc);
    }
    // Pure constructor `ResDtor()` — the second throw-carve-out gate. The
    // destructor itself is NOT carried on the wire (no MetaMemberKind::
    // Destructor); the structural walk on the importer side finds no
    // DestructorSymbol, so the carried verdict is the ONLY source of the
    // non-trivial-dispose answer.
    {
        MetaMember ctor;
        ctor.kind = MetaMemberKind::Constructor;
        ctor.name = "ResDtor";
        ctor.visibility = MetaVisibility::Public;
        ctor.isPure = true;
        resDtor.members.push_back(ctor);
    }
    md.types.push_back(resDtor);
    return md;
}

// The OVER-REJECTION CONTROL's fixture: identical to ResDtor in every way
// EXCEPT `nonTrivialDispose = false` — so the loader leaves
// `importedNonTrivialDispose = false`, the structural walk finds no
// DestructorSymbol (none carried on the wire), the local predicate answers
// "trivial-dispose", and the throw carve-out MUST admit the construction.
ModuleMetadata buildTrivialDisposeLib() {
    ModuleMetadata md;
    md.moduleName = "TrivialLib";
    md.exports = {"PlainExc"};
    md.contractRecordsCarried = true;

    MetaTypeDecl plain;
    plain.name = "PlainExc";
    plain.kind = MetaTypeDeclKind::Class;
    plain.visibility = MetaVisibility::Public;
    plain.nonTrivialDispose = false;  // <-- the difference under test
    plain.builtinExceptionBase = "Exception";
    {
        MetaType exc;
        exc.kind = TypeKind::Named;
        exc.namedName = "Exception";
        plain.baseTypes.push_back(exc);
    }
    {
        MetaMember ctor;
        ctor.kind = MetaMemberKind::Constructor;
        ctor.name = "PlainExc";
        ctor.visibility = MetaVisibility::Public;
        ctor.isPure = true;
        plain.members.push_back(ctor);
    }
    md.types.push_back(plain);
    return md;
}

// Run parse + symbol-collection + type-resolution + expr-typecheck +
// PureChecker on `src` after loading `dep`'s metadata into the importer's
// SemaContext. Mirrors the pure_checker_imported_purity_2054 harness
// ordering so the imported symbols are reconstructed (pass 1+2 in
// loadMetadata) before any importer-side calls are type-checked.
//
// `kCarveOutMsg` is the §22.15 diagnostic fragment that fires only on the
// throw carve-out's path. T1/T2 must report it; T3 must NOT; T4 (single-
// module twin) must report it.
const std::string kCarveOutMsg = "class construction at `throw`";

struct PureCheckResult {
    DiagnosticEngine diagnostics;
    std::unique_ptr<ast::SourceFile> sourceFile;
    std::unique_ptr<sema::SemaContext> ctx;
    bool hasCarveOutError = false;
};

std::unique_ptr<PureCheckResult> runWithImportedDep(
    const ModuleMetadata& dep, std::string_view importerSrc) {
    auto r = std::make_unique<PureCheckResult>();
    Lexer lexer(importerSrc, r->diagnostics);
    Parser parser(lexer, r->diagnostics);
    r->sourceFile = parser.parseCompilationUnit();
    if (!r->sourceFile) return r;
    r->ctx = std::make_unique<sema::SemaContext>(r->diagnostics);
    sema::collectSymbols(*r->sourceFile, *r->ctx);
    // fix #3191 + #3223: loadMetadata publishes the carried trivial-dispose
    // verdict (inverted on the wire) onto `TypeSymbol::importedNonTrivialDispose`,
    // which the fix's local isTrivialDisposeClass now reads.
    loadMetadata(dep, *r->ctx);
    sema::resolveTypes(*r->sourceFile, *r->ctx);
    sema::typecheckExpressions(*r->sourceFile, *r->ctx);
    sema::checkPureBodies(*r->sourceFile, *r->ctx);

    for (const auto& d : r->diagnostics.diagnostics()) {
        if (d.severity == Severity::Error &&
            d.message.find(kCarveOutMsg) != std::string::npos) {
            r->hasCarveOutError = true;
        }
    }
    return r;
}

// The SINGLE-MODULE equivalent of runWithImportedDep — runs the same pipeline
// without any imported metadata. The local predicate's verdict is decided
// structurally (the DestructorSymbol scan) and is independent of the wire.
std::unique_ptr<PureCheckResult> runSingleModule(std::string_view src) {
    auto r = std::make_unique<PureCheckResult>();
    Lexer lexer(src, r->diagnostics);
    Parser parser(lexer, r->diagnostics);
    r->sourceFile = parser.parseCompilationUnit();
    if (!r->sourceFile) return r;
    r->ctx = std::make_unique<sema::SemaContext>(r->diagnostics);
    sema::collectSymbols(*r->sourceFile, *r->ctx);
    sema::resolveTypes(*r->sourceFile, *r->ctx);
    sema::typecheckExpressions(*r->sourceFile, *r->ctx);
    sema::checkPureBodies(*r->sourceFile, *r->ctx);

    for (const auto& d : r->diagnostics.diagnostics()) {
        if (d.severity == Severity::Error &&
            d.message.find(kCarveOutMsg) != std::string::npos) {
            r->hasCarveOutError = true;
        }
    }
    return r;
}

} // namespace

// ===========================================================================
// T1 — §22.15 / §28.9 / §2.8.2: the throw carve-out's trivial-dispose gate
// MUST consult the carried verdict for an IMPORTED non-trivial-dispose class.
//
// Pre-fix: PureChecker's local `isTrivialDisposeClass` scans the imported
// `ResDtor`'s members for a `DestructorSymbol` and finds none (the destructor
// is not part of the wire; #3191's headline finding). Returns true (trivial-
// dispose). The throw carve-out admits the construction. False accept — the
// single-module twin rejects.
//
// Post-fix: the walk consults `importedNonTrivialDispose` at the head and
// returns false (non-trivial-dispose). The throw carve-out rejects.
// ===========================================================================
BEZEL_TEST(xmod_purechecker_3223,
           t1_imported_dtor_class_throw_in_pure_body_rejects_22_15) {
    ModuleMetadata dep = buildDtorLib();
    auto r = runWithImportedDep(
        dep,
        "module App3223T1; import DtorLib; "
        "public class Program "
        "  public pure method Check(low: int, n: int): int "
        "    if n < low then "
        "      throw new ResDtor(); "
        "    end if; "
        "    return n; "
        "  end Check; "
        "  public static method Main(): int => 0; "
        "end Program;");
    // §22.15 throw carve-out's first gate: ResDtor is non-trivial-dispose,
    // so the throw is rejected.
    BEZEL_EXPECT_TRUE(r->hasCarveOutError);
}

// ===========================================================================
// T1b — the SINGLE-MODULE TWIN of T1. §22.15 must agree across the boundary:
// the local predicate answers non-trivial-dispose structurally (it sees the
// DestructorSymbol), and the cross-module test must agree now that it
// consults the carried verdict.
//
// This row is what makes T1 a CROSS-MODULE claim — it was already green
// pre-fix, and it is what the two-module build must now agree with.
// ===========================================================================
BEZEL_TEST(xmod_purechecker_3223,
           t1b_single_module_twin_dtor_class_throw_rejects_22_15) {
    auto r = runSingleModule(
        "module Loc3223T1; "
        "public class ResL "
        "  destructor "
        "  end destructor; "
        "  public pure constructor() "
        "  end constructor; "
        "end ResL; "
        "public class Program "
        "  public pure method Check(low: int, n: int): int "
        "    if n < low then "
        "      throw new ResL(); "
        "    end if; "
        "    return n; "
        "  end Check; "
        "  public static method Main(): int => 0; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r->hasCarveOutError);
}

// ===========================================================================
// T2 — the same shape at a DERIVED local class over an imported non-trivial
// base. The fix's local predicate consults `importedNonTrivialDispose` PER
// LEVEL, not once at the head, so the verdict is found when the walk REACHES
// the imported base.
//
// The local class `DervLocal : ResDtor` has no destructor and no `owned`
// field of its own, so without the carried verdict the walk would answer
// "trivial-dispose" at the LOCAL level and never reach the imported base —
// a §22.15 false accept. The fix routes the predicate to the imported level
// and the throw is rejected.
// ===========================================================================
BEZEL_TEST(xmod_purechecker_3223,
           t2_local_derived_over_imported_dtor_base_throw_rejects_22_15) {
    ModuleMetadata dep = buildDtorLib();
    auto r = runWithImportedDep(
        dep,
        "module App3223T2; import DtorLib; "
        "public class DervLocal: ResDtor "
        "  public pure constructor() "
        "    base(); "
        "  end constructor; "
        "end DervLocal; "
        "public class Program "
        "  public pure method Check(low: int, n: int): int "
        "    if n < low then "
        "      throw new DervLocal(); "
        "    end if; "
        "    return n; "
        "  end Check; "
        "  public static method Main(): int => 0; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r->hasCarveOutError);
}

// ===========================================================================
// T3 — OVER-REJECTION CONTROL: an imported TRIVIAL-DISPOSE class must still
// be admitted by the throw carve-out. This pins the boundary — the fix must
// not silently admit non-trivial carried types AND must not over-fire by
// treating every import as non-trivial.
//
// A regression to "always reject import-throw in a pure body" would break
// this row; a regression to "always admit" would break T1/T1b/T2.
// ===========================================================================
BEZEL_TEST(xmod_purechecker_3223,
           t3_imported_trivial_dispose_class_throw_admitted_22_15) {
    ModuleMetadata dep = buildTrivialDisposeLib();
    auto r = runWithImportedDep(
        dep,
        "module App3223T3; import TrivialLib; "
        "public class Program "
        "  public pure method Check(low: int, n: int): int "
        "    if n < low then "
        "      throw new PlainExc(); "
        "    end if; "
        "    return n; "
        "  end Check; "
        "  public static method Main(): int => 0; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r->hasCarveOutError);
}

} // namespace bezel::module
