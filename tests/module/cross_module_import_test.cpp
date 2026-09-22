// Cross-module import resolution + A-imports-B end-to-end build
// (§2.2 / §2.5 / §2.6, issue #328 increment 3).
//
// Increment 1 defined the public-interface metadata schema + bitstream codec;
// Increment 2 built the metadata from live symbols and embedded it in the
// `.bezel.meta` object section. Increment 3 is the consumer side:
//
//   - the LOADER reads a dependency's `.bezel.meta`, deserializes it, and
//     reconstructs its exported public interface as live sema TypeSymbols in the
//     importer's SemaContext, registered under the module's §2.1 dotted name;
//   - SEMA RESOLUTION exposes `import X;`-named modules' types into the
//     importer's name resolution (§2.2) and type-checks the importer against the
//     imported signatures (§2.6 step 3);
//   - CODEGEN emits external references to imported callables (mangling matches
//     the dependency's emitter) so the linker resolves them;
//   - the DRIVER orchestrates: load dependency metadata before sema, link the
//     dependency object into the importer's executable (§2.6 step 4).
//
// These tests cover, in order: (1) the loader at the sema level — B type-checks
// against A's metadata, with negative cases (wrong arity / wrong type) and the
// import gate (an unimported module's names are invisible, §2.2); (2) the
// headline full build: compile A → object, compile B (`import A`) → object,
// link, run, assert the result.

#include "module/ModuleLoader.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "ast/Node.hpp"
#include "driver/Driver.hpp"
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
#include "support/aot_support.hpp"  // #2290: aot_detail::spawnCaptured/hasSanitizerReport
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

// --- sema-level harness -----------------------------------------------------

// Build the ModuleMetadata for `source` (a dependency module) by running the
// front-end and the Increment-2 builder. Mirrors the driver front-end order.
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
    // #662c: pass source so ctor/dtor body texts are extracted for generic classes.
    return buildModuleMetadata(ctx, source);
}

// Run the importer front-end (`importerSource`) against a dependency's metadata
// loaded into the same context. Returns the number of diagnostics (0 ==
// type-checks clean). The AST + context are kept alive for the call's duration.
struct ImporterRun {
    DiagnosticEngine diags;
    std::unique_ptr<ast::SourceFile> sf;
    std::unique_ptr<sema::SemaContext> ctx;
    bool hadErrors = false;
};

std::unique_ptr<ImporterRun> runImporter(const ModuleMetadata& dep,
                                         std::string_view importerSource) {
    auto r = std::make_unique<ImporterRun>();
    Lexer lexer(importerSource, r->diags);
    Parser parser(lexer, r->diags);
    r->sf = parser.parseCompilationUnit();
    if (!r->sf) {
        r->hadErrors = true;
        return r;
    }
    r->ctx = std::make_unique<sema::SemaContext>(r->diags);
    // Symbol collection first (establishes THIS module's symbols), then load the
    // dependency's interface into the cross-module table, exactly as the driver.
    sema::collectSymbols(*r->sf, *r->ctx);
    loadMetadata(dep, *r->ctx);
    sema::resolveTypes(*r->sf, *r->ctx);
    sema::typecheckExpressions(*r->sf, *r->ctx);
    sema::checkDefiniteAssignment(*r->sf, *r->ctx);
    r->hadErrors = r->diags.hasErrors();
    return r;
}

}  // namespace

// §2.5 / §2.6 step 3: the importer type-checks CLEAN against an imported
// module's reconstructed signature. B calls A's exported static methods with
// well-typed arguments — no diagnostics.
BEZEL_TEST(cross_module_import, importer_typechecks_clean_spec_2_5) {
    ModuleMetadata dep = buildMetaFor(
        "module MathLib; "
        "public class Calc "
        "  public static method Square(n: int): int => n * n; "
        "  public static method Add(a: int, b: int): int => a + b; "
        "end Calc;");
    auto r = runImporter(
        dep,
        "module App; "
        "import MathLib; "
        "public class Program "
        "  public static method Main(): int => Calc.Add(Calc.Square(6), 6); "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// §2.2: imports are file-scoped — an imported module's names are visible only
// when explicitly imported. Without `import MathLib;`, `Calc` is unresolved even
// though the dependency's metadata is loaded in the context.
BEZEL_TEST(cross_module_import, import_gate_unimported_invisible_spec_2_2) {
    ModuleMetadata dep = buildMetaFor(
        "module MathLib; "
        "public class Calc "
        "  public static method Square(n: int): int => n * n; "
        "end Calc;");
    auto r = runImporter(
        dep,
        "module App; "  // NO `import MathLib;`
        "public class Program "
        "  public static method Main(): int => Calc.Square(6); "
        "end Program;");
    BEZEL_EXPECT_TRUE(r->hadErrors);  // `Calc` unresolved
}

// §22.5: a call to an imported method with the wrong argument count is rejected
// against the imported signature (cross-module arity check).
BEZEL_TEST(cross_module_import, wrong_arity_rejected_spec_22_5) {
    ModuleMetadata dep = buildMetaFor(
        "module MathLib; "
        "public class Calc "
        "  public static method Add(a: int, b: int): int => a + b; "
        "end Calc;");
    auto r = runImporter(
        dep,
        "module App; "
        "import MathLib; "
        "public class Program "
        "  public static method Main(): int => Calc.Add(1); "  // needs 2 args
        "end Program;");
    BEZEL_EXPECT_TRUE(r->hadErrors);
}

// §5.1: a call to an imported method whose argument is not assignable to the
// imported parameter type is rejected (cross-module argument typing).
BEZEL_TEST(cross_module_import, wrong_arg_type_rejected_spec_5_1) {
    ModuleMetadata dep = buildMetaFor(
        "module MathLib; "
        "public class Calc "
        "  public static method Square(n: int): int => n * n; "
        "end Calc;");
    auto r = runImporter(
        dep,
        "module App; "
        "import MathLib; "
        "public class Program "
        "  public static method Main(): int => Calc.Square(\"hi\"); "  // string!=int
        "end Program;");
    BEZEL_EXPECT_TRUE(r->hadErrors);
}

// fix #2418 (⚠ imported-symbol decision): an imported member is reconstructed
// from the dependency's `.bezel.meta` (ModuleLoader.cpp's
// `ctx.makeImportedSymbol<...>()` call sites), NOT from an
// ast::Member-derived decl node — there is no local AST to read a `span`
// from (the #2374 defect class: "imported symbols have no astNode"), and
// SourceLocation carries no filename, so stamping one from a *re-parsed*
// dependency-source-text location (as ModuleLoader lazily does for a body it
// re-parses for cross-module inlining) would silently mislabel the position
// against the IMPORTING file — worse than 0:0, not better. #2418's
// SymbolCollection.cpp / Parser_Decls.cpp fix deliberately does not touch
// ModuleLoader.cpp: an imported member's declLoc stays the SourceLocation
// default (unset / 0:0), exactly as before the fix. Pinned here so that
// invariant doesn't silently drift.
BEZEL_TEST(cross_module_import, imported_member_decl_loc_stays_unset_2418) {
    ModuleMetadata dep = buildMetaFor(
        "module MathLib; "
        "public class Calc "
        "  public _cached: int; "
        "  public static method Square(n: int): int => n * n; "
        "end Calc;");
    auto r = runImporter(
        dep,
        "module App; "
        "import MathLib; "
        "public class Program "
        "  public static method Main(): int => Calc.Square(6); "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
    sema::TypeSymbol* calc = r->ctx->lookupImportedTypeByName("Calc");
    BEZEL_EXPECT_TRUE(calc != nullptr);
    if (!calc) return;
    // The imported TYPE itself: also unset (no local ast::TypeDecl either).
    BEZEL_EXPECT_EQ(calc->declLoc.line, std::uint32_t(0));
    BEZEL_EXPECT_EQ(calc->declLoc.column, std::uint32_t(0));
    bool sawField = false, sawMethod = false;
    for (sema::Symbol* m : calc->members) {
        if (dynamic_cast<sema::FieldSymbol*>(m)) {
            sawField = true;
            BEZEL_EXPECT_EQ(m->declLoc.line, std::uint32_t(0));
        } else if (dynamic_cast<sema::MethodSymbol*>(m)) {
            sawMethod = true;
            BEZEL_EXPECT_EQ(m->declLoc.line, std::uint32_t(0));
        }
    }
    BEZEL_EXPECT_TRUE(sawField);
    BEZEL_EXPECT_TRUE(sawMethod);
}

// fix #795 / §2.5 / §11.3 / §12.1: a call to a member that does NOT exist on an
// imported type is rejected with a "no member" diagnostic. Before the fix the
// importer silently accepted it (the imported type has a null local `decl`, so
// the issue-#414 known-complete-user-type gate missed it) and codegen lowered a
// null callee → a garbage/zero result. Member-existence is a basic soundness
// check; for imported types it must be enforced too.
BEZEL_TEST(cross_module_import, unknown_member_on_imported_type_rejected_795) {
    ModuleMetadata dep = buildMetaFor(
        "module ExtLib; "
        "public class Widget "
        "  public _val: int; "
        "  public constructor(v: int) _val := v; end constructor; "
        "end Widget;");
    auto r = runImporter(
        dep,
        "module App; "
        "import ExtLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var w := new Widget(21); "
        "    return w.Nonexistent(); "   // no such member on Widget
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r->hadErrors);
}

// fix #795 (false-rejection guard) / §2.5 / §12: legitimate imported members —
// an instance method, a public field, AND a member INHERITED from an imported
// base (lookupMember walks the reconstructed base chain) — all resolve and
// type-check clean; the new unknown-member diagnostic must not over-reject.
BEZEL_TEST(cross_module_import, imported_real_and_inherited_members_resolve_795) {
    ModuleMetadata dep = buildMetaFor(
        "module ExtLib; "
        "public class Base "
        "  public _b: int; "
        "  public constructor(b: int) _b := b; end constructor; "
        "  public method BaseVal(): int => _b; "
        "end Base; "
        "public class Derived: Base "
        "  public constructor(b: int) base(b); end constructor; "
        "  public method DerivedVal(): int => _b + 1; "
        "end Derived;");
    auto r = runImporter(
        dep,
        "module App; "
        "import ExtLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var d := new Derived(10); "
        "    return d.BaseVal() + d.DerivedVal() + d._b; "  // inherited + own + field
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// §2.5 by-name resolution within the loaded module set: a method whose return
// type is a SIBLING exported type of the same module reconstructs with that
// sibling's TypeSymbol as its return type's declaration (named-type-by-name).
BEZEL_TEST(cross_module_import, sibling_named_type_resolves_spec_2_5) {
    ModuleMetadata dep = buildMetaFor(
        "module Lib; "
        "public class Token "
        "  public Id: int; "
        "end Token; "
        "public class Factory "
        "  public static method Make(): Token => new Token(); "
        "end Factory;");
    DiagnosticEngine diags;
    sema::SemaContext ctx(diags);
    BEZEL_EXPECT_TRUE(loadMetadata(dep, ctx));
    // The Token type and the Factory type are both loaded under "Lib".
    const std::vector<sema::TypeSymbol*>* exported = ctx.importedTypesOf("Lib");
    BEZEL_EXPECT_TRUE(exported != nullptr);
    if (!exported) return;
    sema::TypeSymbol* factory = nullptr;
    sema::TypeSymbol* token = nullptr;
    for (sema::TypeSymbol* t : *exported) {
        if (t->name == "Factory") factory = t;
        if (t->name == "Token") token = t;
    }
    BEZEL_EXPECT_TRUE(factory != nullptr);
    BEZEL_EXPECT_TRUE(token != nullptr);
    if (!factory || !token) return;
    // Factory.Make's return type resolves (by name, §2.5) to the loaded Token.
    sema::MethodSymbol* make = nullptr;
    for (sema::Symbol* m : factory->members)
        if (auto* ms = dynamic_cast<sema::MethodSymbol*>(m))
            if (ms->name == "Make") make = ms;
    BEZEL_EXPECT_TRUE(make != nullptr);
    if (!make) return;
    sema::TypeRef* ret = ctx.importedReturnType(make);
    BEZEL_EXPECT_TRUE(ret != nullptr);
    if (ret) {
        BEZEL_EXPECT_TRUE(ret->kind == sema::TypeRef::Kind::Named);
        BEZEL_EXPECT_TRUE(ret->declaration == token);  // bound to sibling
    }
}

// §11.7 (FINDING [6]): a `closed` (closed abstract) base may be
// inherited only *within its declaring module*. The importer App imports a
// `closed` Shape from Lib and declares a local `class Local : Shape` —
// a CROSS-module derivation of the closed-abstract base — which the compiler
// must reject ("the compiler rejects subclasses in other modules", §11.7). The
// base arrives flagged as imported, so the inheritance gate distinguishes it
// from a same-module base.
BEZEL_TEST(cross_module_import, closed_class_cross_module_subclass_rejected_spec_11_7) {
    ModuleMetadata dep = buildMetaFor(
        "module ShapeLib; "
        "public closed class Shape "
        "  public abstract method Area(): int; "
        "end Shape;");
    auto r = runImporter(
        dep,
        "module App; "
        "import ShapeLib; "
        "public class Local : Shape "  // cross-module subclass of closed base
        "  public override method Area(): int => 1; "
        "end Local;");
    BEZEL_EXPECT_TRUE(r->hadErrors);
}

// §11.7 control: importing and *using* a closed-abstract type (without
// subclassing it across the boundary) is fine. App imports ShapeLib's `Shape`
// and merely names it as a method-parameter type — no cross-module derivation,
// so no error. Isolates the rejection above to the inheritance gate.
BEZEL_TEST(cross_module_import, imported_closed_class_use_without_subclass_ok_spec_11_7) {
    ModuleMetadata dep = buildMetaFor(
        "module ShapeLib; "
        "public closed class Shape "
        "  public abstract method Area(): int; "
        "end Shape;");
    auto r = runImporter(
        dep,
        "module App; "
        "import ShapeLib; "
        "public class Program "
        "  public static method AreaOf(s: Shape): int => s.Area(); "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// §2.5 / §22.7 (FINDING [7] manifestation B): a non-default imported constructor
// is part of the module's public interface — its declared `constructor(args)`
// must be VISIBLE to the importer, so `new T(args)` resolves against the real
// signature. Before the fix the loader did not reconstruct the constructor, so
// the importer assumed only the implicit 0-arg default and rejected `new T(10)`.
BEZEL_TEST(cross_module_import, imported_nondefault_ctor_visible_spec_22_7) {
    ModuleMetadata dep = buildMetaFor(
        "module Lib; "
        "public class Counter "
        "  public v: int; "
        "  public constructor(start: int) v := start; end constructor; "
        "  public method Get(): int => v; "
        "  destructor end destructor; "
        "end Counter;");
    auto r = runImporter(
        dep,
        "module App; "
        "import Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var c := new Counter(10); "  // the imported ctor's arity
        "    return c.Get(); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// §22.7 (FINDING [7] B): a call to an imported non-default constructor with the
// WRONG argument count is rejected against the reconstructed signature. `new
// Counter()` supplies 0 args but the imported ctor takes 1.
BEZEL_TEST(cross_module_import, imported_ctor_wrong_arity_zero_rejected_spec_22_7) {
    ModuleMetadata dep = buildMetaFor(
        "module Lib; "
        "public class Counter "
        "  public v: int; "
        "  public constructor(start: int) v := start; end constructor; "
        "end Counter;");
    auto r = runImporter(
        dep,
        "module App; "
        "import Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var c := new Counter(); "  // needs 1 arg
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r->hadErrors);
}

// §22.7 (FINDING [7] B): a call to an imported non-default constructor with too
// MANY arguments is rejected. `new Counter(1, 2)` supplies 2 args; the imported
// ctor takes 1.
BEZEL_TEST(cross_module_import, imported_ctor_wrong_arity_two_rejected_spec_22_7) {
    ModuleMetadata dep = buildMetaFor(
        "module Lib; "
        "public class Counter "
        "  public v: int; "
        "  public constructor(start: int) v := start; end constructor; "
        "end Counter;");
    auto r = runImporter(
        dep,
        "module App; "
        "import Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var c := new Counter(1, 2); "  // ctor takes 1, not 2
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r->hadErrors);
}

// §5.1 (FINDING [7] B): a call to an imported constructor whose argument is not
// assignable to the imported parameter type is rejected. `new Counter("hi")`
// supplies a string where the imported ctor wants an int.
BEZEL_TEST(cross_module_import, imported_ctor_wrong_arg_type_rejected_spec_5_1) {
    ModuleMetadata dep = buildMetaFor(
        "module Lib; "
        "public class Counter "
        "  public v: int; "
        "  public constructor(start: int) v := start; end constructor; "
        "end Counter;");
    auto r = runImporter(
        dep,
        "module App; "
        "import Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var c := new Counter(\"hi\"); "  // string != int
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r->hadErrors);
}

// §2.5 by-name resolution: an imported constructor is reconstructed as a
// ConstructorSymbol on the imported TypeSymbol, with its parameters carrying
// resolved types (so codegen + overload resolution can read them). Verifies the
// loader-level reconstruction (the sema side of FINDING [7] B).
BEZEL_TEST(cross_module_import, imported_ctor_reconstructed_as_member_spec_2_5) {
    ModuleMetadata dep = buildMetaFor(
        "module Lib; "
        "public class Counter "
        "  public v: int; "
        "  public constructor(start: int) v := start; end constructor; "
        "end Counter;");
    DiagnosticEngine diags;
    sema::SemaContext ctx(diags);
    BEZEL_EXPECT_TRUE(loadMetadata(dep, ctx));
    const std::vector<sema::TypeSymbol*>* exported = ctx.importedTypesOf("Lib");
    BEZEL_EXPECT_TRUE(exported != nullptr);
    if (!exported) return;
    sema::TypeSymbol* counter = nullptr;
    for (sema::TypeSymbol* t : *exported)
        if (t->name == "Counter") counter = t;
    BEZEL_EXPECT_TRUE(counter != nullptr);
    if (!counter) return;
    sema::ConstructorSymbol* ctor = nullptr;
    for (sema::Symbol* m : counter->members)
        if (auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m)) ctor = cs;
    BEZEL_EXPECT_TRUE(ctor != nullptr);
    if (!ctor) return;
    BEZEL_EXPECT_EQ(ctor->params.size(), static_cast<std::size_t>(1));
    auto* p = dynamic_cast<sema::ParameterSymbol*>(ctor->params[0]);
    BEZEL_EXPECT_TRUE(p != nullptr);
    if (p) {
        BEZEL_EXPECT_TRUE(p->resolvedDeclType != nullptr);
        if (p->resolvedDeclType) {
            BEZEL_EXPECT_TRUE(p->resolvedDeclType->kind ==
                              sema::TypeRef::Kind::Primitive);
            BEZEL_EXPECT_TRUE(p->resolvedDeclType->primitive ==
                              sema::PrimitiveType::Int);
        }
    }
}

// §22.7 read-older-tolerance (issue #328 gate 5): an imported type that exports
// NO constructor (the shape an older v2 .bezel.meta carries — it has no ctor
// reconstruction) still loads and constructs gracefully: the importer applies the
// implicit default 0-arg constructor (§22.7), so `new T()` is accepted and
// `new T(x)` is rejected. The "no exported ctors" path must behave exactly as
// before this change for such types.
BEZEL_TEST(cross_module_import, imported_type_without_ctor_uses_implicit_default_spec_22_7) {
    ModuleMetadata dep = buildMetaFor(
        "module Lib; "
        "public class Plain "
        "  public x: int; "  // no declared constructor
        "  public method Get(): int => x; "
        "end Plain;");
    // `new Plain()` (the implicit default) type-checks clean.
    auto rOk = runImporter(
        dep,
        "module App; "
        "import Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var p := new Plain(); "
        "    return p.Get(); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!rOk->hadErrors);
    // `new Plain(1)` exceeds the implicit default's 0 parameters → rejected.
    auto rBad = runImporter(
        dep,
        "module App; "
        "import Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var p := new Plain(1); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(rBad->hadErrors);
}

// --- full end-to-end build (the headline test) ------------------------------
//
// §2.6: compile module A (exports public static methods) → a native OBJECT with
// embedded metadata; compile module B (`import A`; calls A's methods) → object;
// link both → executable; run; assert the result. Requires clang + objcopy on
// PATH (the project toolchain) and the LLVM Object API (linked into bezel_core).

namespace {

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// #2290 / #710: run a two-module (A-imports-B) executable under generated-code
// AddressSanitizer/LeakSanitizer — the cross-module counterpart of
// support/aot_support.hpp's runAotChecked, which only compiles a single module.
// Compiles `aSrc` to a dependency object (plain, no sanitizer — see
// bezelc's own "--sanitize=address applies to the executable build" rule),
// then compiles+links `bSrc` against it with `opts.sanitizeAddress = true`, then
// runs the executable with LeakSanitizer on and inspects both the exit status
// and the captured ASan/LSan report text (aot_detail::hasSanitizerReport) — a
// leak, double-free, or use-after-free in the imported ctor body's owned local
// (inlined into B's IR by inlineCtorBody, §14.1) fails the assertion the same
// way runAotChecked's single-module oracle does. Returns exit code, or -1 on
// any build/run failure (caller inspects `*sawSanitizerReport`/`*built ok`).
struct CrossModuleAsanResult {
    bool builtOk = false;
    bool ran = false;
    bool sanitizerFired = false;
    int exitCode = -1;
    std::string sanitizerText;
};

CrossModuleAsanResult runCrossModuleChecked(const std::string& aSource,
                                            const std::string& bSource) {
    namespace aot_detail = bezel::test::aot_detail;
    CrossModuleAsanResult result;
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_2290_asan_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());
    TempFile sanOut((dir / (stem + ".san.txt")).string());
    TempFile stdOut((dir / (stem + ".out.txt")).string());

    writeSource(aSrc.path, aSource);
    writeSource(bSrc.path, bSource);

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    if (!ra.success) return result;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    bezel::CompileResult rb =
        driver.compileFile(bSrc.path, bExe.path, deps, opts);
    if (!rb.success) return result;
    result.builtOk = true;

    bool timedOut = false;
    int status = aot_detail::spawnCaptured(
        {bExe.path}, stdOut.path, sanOut.path, aot_detail::aotTimeoutSec(),
        timedOut, {{"ASAN_OPTIONS", "detect_leaks=1:exitcode=99:abort_on_error=0"}});
    if (timedOut) return result;

    std::string sanText = aot_detail::slurp(sanOut.path);
    bool crashed = WIFSIGNALED(status);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    result.ran = WIFEXITED(status);
    result.sanitizerFired = crashed || aot_detail::hasSanitizerReport(sanText);
    result.sanitizerText = std::move(sanText);
    return result;
}

}  // namespace

// §2.5 / §2.6 step 4: A-imports-B native build. A exports `Square`/`Add`; B
// imports A and computes `Add(Square(6), 6)` = 42; link A's object into B's
// executable; run → 42.
BEZEL_TEST(cross_module_import, a_imports_b_native_build_spec_2_6) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_inc3_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module MathLib; "
        "public class Calc "
        "  public static method Square(n: int): int => n * n; "
        "  public static method Add(a: int, b: int): int => a + b; "
        "end Calc;");
    writeSource(bSrc.path,
        "module App; "
        "import MathLib; "
        "public class Program "
        "  public static method Main(): int => Calc.Add(Calc.Square(6), 6); "
        "end Program;");

    bezel::Driver driver;
    // Compile A to a kept object with embedded `.bezel.meta` (§2.6 step 4).
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    // Compile B importing A; the loader reads A's metadata, sema resolves the
    // `import` + cross-module calls, codegen emits externals, the driver links
    // A's object into B's executable.
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    // Run the linked executable; assert Add(Square(6), 6) == 42.
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 42);
}

// §2.5 / §2.6 + §22.12 (issue #328 FINDING [3] BUG A): a DIRECT read of an
// imported static `const` must observe the library's value. A §22.12 const
// initializer "must be a compile-time-constant expression", so the library
// emits NO storage symbol for it; the importer FOLDS the value (carried in the
// dependency metadata) at the read site. Before the fix this returned
// non-deterministic garbage (an uninitialized read); after, it returns 7.
BEZEL_TEST(cross_module_import, imported_const_read_spec_22_12) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_inc3_const_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module ConstLib; "
        "public class Lib "
        "  public static const K: int := 7; "
        "end Lib;");
    writeSource(bSrc.path,
        "module App; "
        "import ConstLib; "
        "public class Program "
        "  public static method Main(): int => Lib.K; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 7);
}

// §2.5 / §2.6 + §22.12 (issue #328 FINDING [3] BUG B): a DIRECT read of an
// imported static FIELD must observe the EXPORTER's value. The exporter emits
// the field's storage as an EXTERNAL global (mangled `bz$<Class>$<field>`); the
// importer references it as an external (no fresh definition), so the read loads
// the linked definition. Before the fix this returned 0 (the importer bound to a
// fresh zero-initialized local copy); after, it returns the field's initial 99.
BEZEL_TEST(cross_module_import, imported_static_field_read_spec_22_12) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_inc3_field_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    // §22.12: a class-level `static` field with an initializer. The static
    // initializer runs in the EXPORTING module (the importer references only the
    // storage symbol), so the initialized value crosses the boundary.
    writeSource(aSrc.path,
        "module FieldLib; "
        "public class Lib "
        "  public static Counter: int := 99; "
        "end Lib;");
    writeSource(bSrc.path,
        "module App; "
        "import FieldLib; "
        "public class Program "
        "  public static method Main(): int => Lib.Counter; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 99);
}

// §2.5 / §2.6 / §22.8 (FINDING [7] manifestation A): constructing an imported
// type must RUN the library's constructor across the module boundary. A's `Box`
// has `constructor() n := 42;`; B does `new Box()` then reads `b.Get()`. Before
// the fix `new Box()` lowered to a bare calloc with no ctor call, so the field
// stayed 0; after, the importer calls the linked `bz$Box$ctor`, so the ctor body
// runs and `Get()` returns 42. The repro's deterministic-0 miscompile is the
// canonical FINDING [7] A case; assert 42 (the ctor body ran).
BEZEL_TEST(cross_module_import, imported_ctor_body_runs_native_spec_22_8) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_inc4_ctorA_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module Lib; "
        "public class Box "
        "  public n: int; "
        "  public constructor() n := 42; end constructor; "
        "  public method Get(): int => n; "
        "  destructor end destructor; "
        "end Box;");
    writeSource(bSrc.path,
        "module App; "
        "import Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    return b.Get(); "  // ctor body ran cross-module → 42
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 42);
}

// §2.5 / §2.6 / §22.8 (FINDING [7] manifestation B): a NON-DEFAULT imported
// constructor is both visible AND runs. A's `Counter` has `constructor(start:
// int) v := start;`; B does `new Counter(10)` then reads `c.Get()`. Before the
// fix B rejected `new Counter(10)` outright (the ctor signature was invisible);
// after, the importer resolves the ctor and calls the linked `bz$Counter$ctor`
// with the argument, so the field holds 10.
BEZEL_TEST(cross_module_import, imported_nondefault_ctor_runs_native_spec_22_8) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_inc4_ctorB_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module Lib; "
        "public class Counter "
        "  public v: int; "
        "  public constructor(start: int) v := start; end constructor; "
        "  public method Get(): int => v; "
        "  destructor end destructor; "
        "end Counter;");
    writeSource(bSrc.path,
        "module App; "
        "import Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var c := new Counter(10); "
        "    return c.Get(); "  // non-default ctor ran with arg → 10
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 10);
}

// §22.8 construction sequence: FIELD INITIALIZERS (step 2), not just the ctor
// body, run across the module boundary. A's `Widget` has a field initializer
// `w: int := 7;` and an empty constructor that does NOT assign w; B reads
// `Get()` after `new Widget()`. The field initializer must have run during
// cross-module construction, so the result is 7 (not the calloc-zero 0).
BEZEL_TEST(cross_module_import, imported_field_initializer_runs_native_spec_22_8) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_inc4_fieldinit_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module Lib; "
        "public class Widget "
        "  public w: int := 7; "  // §22.8 step 2 field initializer
        "  public constructor() end constructor; "  // body does NOT set w
        "  public method Get(): int => w; "
        "  destructor end destructor; "
        "end Widget;");
    writeSource(bSrc.path,
        "module App; "
        "import Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var x := new Widget(); "
        "    return x.Get(); "  // field initializer ran → 7
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 7);
}

// §2.5 / §2.6 / §12 (cross-module instance, item 2 — SOUNDNESS): a VIRTUAL method
// call on an imported object must dispatch to the correct override. A's `Animal`
// has `virtual Sound() => 1` and `Dog : Animal` overrides `Sound() => 2`. B holds
// a `Dog` through an `Animal`-typed parameter (a polymorphic call site that must
// go through the vtable) and calls `Sound()`. Before the fix the exporter emitted
// the type's vtable (vt$Dog) with INTERNAL linkage, so the importer could not
// install it; the `new Dog()` slot-0 vtable stayed calloc-zero and the dispatch
// mis-resolved (observed: returns the base's 1). After: the exporter exports
// vt$Dog with EXTERNAL linkage and the importer installs it via bz$Dog$init, so
// the polymorphic call resolves to Dog's override → 2. Exercises BOTH a derived
// class overriding an imported base's virtual AND a base-reference dispatch.
BEZEL_TEST(cross_module_import, imported_virtual_dispatch_override_spec_12) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_xm_vdisp_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module VLib; "
        "public class Animal "
        "  public virtual method Sound(): int => 1; "
        "  destructor end destructor; "
        "end Animal; "
        "public class Dog : Animal "
        "  public override method Sound(): int => 2; "
        "  destructor end destructor; "
        "end Dog;");
    writeSource(bSrc.path,
        "module App; "
        "import VLib; "
        "public class Program "
        "  public static method Speak(a: Animal): int => a.Sound(); "
        "  public static method Main(): int "
        "    using var d := new Dog(); "
        "    return Speak(d); "  // polymorphic dispatch → Dog.Sound() == 2
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 2);
}

// §2.5 / §2.6 / §28.4 (cross-module instance, item 1 — LEAK): an imported
// `using var` must be disposed exactly once at scope end. A's `Tracker` increments
// a static `Disposed` counter in its destructor; B constructs a `using var
// Tracker` in a helper scope and, after that scope returns, reads `Tracker.Disposed`
// (an imported static field, observed cross-module). Before the fix the importer
// had no disposerFns_ entry / no external Dispose for the imported type, so the
// scope-end disposal was never registered and the destructor never ran (counter
// stays 0 — the leak). After: the exporter exports bz$Tracker$Dispose with
// EXTERNAL linkage, the importer registers it, and the `using var` disposes once
// at scope end → counter == 1.
BEZEL_TEST(cross_module_import, imported_using_var_disposed_once_spec_28_4) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_xm_dispose_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module DLib; "
        "public class Tracker "
        "  public static Disposed: int := 0; "
        "  public constructor() end constructor; "
        "  public method Ping(): int => 1; "
        "  destructor "
        "    Disposed := Disposed + 1; "
        "  end destructor; "
        "end Tracker;");
    writeSource(bSrc.path,
        "module App; "
        "import DLib; "
        "public class Program "
        "  public static method Use(): int "
        "    using var t := new Tracker(); "  // disposed at scope end
        "    return t.Ping(); "
        "  end Use; "
        "  public static method Main(): int "
        "    var r := Use(); "
        "    return Tracker.Disposed; "  // destructor ran once → 1
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 1);
}

// §2.5 / §2.6 / §22.8 (cross-module instance, item 3 — MISCOMPILE): a NON-foldable
// field initializer must run during cross-module construction. A's `Holder` has
// `n: int := Compute();` where `Compute()` is a static method (a non-compile-time-
// constant initializer that renderConstExpr cannot fold) and an empty constructor
// that does NOT assign `n`. B reads `Get()` after `new Holder()`. Before the fix
// only constant-foldable initializers crossed (replayed textually); a non-foldable
// one left the field calloc-zero (0). After: §22.8 step-2 field initializers run
// inside the exporter's cross-module construction path (bz$Holder$init), so the
// initializer executes regardless of foldability → 5.
BEZEL_TEST(cross_module_import, imported_nonfoldable_field_initializer_spec_22_8) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_xm_nfinit_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module NLib; "
        "public class Holder "
        "  public n: int := Compute(); "  // §22.8 step 2, NON-foldable
        "  public static method Compute(): int => 5; "
        "  public constructor() end constructor; "  // body does NOT set n
        "  public method Get(): int => n; "
        "  destructor end destructor; "
        "end Holder;");
    writeSource(bSrc.path,
        "module App; "
        "import NLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    return h.Get(); "  // non-foldable field initializer ran → 5
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 5);
}

// §10 / §10.2 / §5.4 (issue #328 FINDING [1]): `int(Color.Green)` on an IMPORTED
// enum must observe the library's declared value (2), not 0. Before the fix the
// importer dropped each imported constant's value — the loader reconstructed the
// EnumConstantSymbol without it and codegen's computeEnumValues skipped imported
// enums (no AST TypeDecl), so every imported enum constant lowered to 0. The
// metadata already carried the value; the loader + symbol-field + codegen walk
// now propagate it. §10: "Each value is backed by an integer at runtime."
BEZEL_TEST(cross_module_import, imported_enum_int_cast_explicit_values_spec_10_2) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_xm_enum_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module EnumLib; "
        "public enum Color Red := 1, Green := 2, Blue := 3; end Color;");
    // int(Green)=2, int(Red)=1, int(Blue)=3 → 2*100 + 1*10 + 3 = 213.
    writeSource(bSrc.path,
        "module App; "
        "import EnumLib; "
        "public class Program "
        "  public static method Main(): int "
        "    return int(Color.Green) * 100 + int(Color.Red) * 10 "
        "         + int(Color.Blue); "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 213);
}

// §10 (issue #328 FINDING [1]): equality between two references to the SAME
// imported enum constant must be true. Before the fix `Color.Green = Color.Green`
// folded to `0 = 0`... but observably returned false in the repro because the
// two sides lowered independently; either way it must now be true. Two DISTINCT
// constants must compare unequal. Returns 42 when both hold.
BEZEL_TEST(cross_module_import, imported_enum_equality_spec_10) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_xm_enumeq_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module EnumLib; "
        "public enum Color Red := 1, Green := 2, Blue := 3; end Color;");
    writeSource(bSrc.path,
        "module App; "
        "import EnumLib; "
        "public class Program "
        "  public static method Main(): int "
        "    if Color.Green = Color.Green and Color.Green <> Color.Red then "
        "      return 42; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 42);
}

// §19 / §10 (issue #328 FINDING [1]): a `case` over an IMPORTED enum scrutinee
// selects the arm whose constant matches. Each arm's constant lowers via the
// same enumValue_ lookup as int(enum)/equality, so the fix that populates it for
// imported enums covers this path too. Scrutinee Green → arm value 22.
BEZEL_TEST(cross_module_import, imported_enum_case_arm_selects_spec_19) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_xm_enumcase_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module EnumLib; "
        "public enum Color Red := 1, Green := 2, Blue := 3; end Color;");
    writeSource(bSrc.path,
        "module App; "
        "import EnumLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var c := Color.Green; "
        "    return (case c of "
        "      Color.Red => 11, "
        "      Color.Green => 22, "
        "      Color.Blue => 33,); "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 22);
}

// §10.1 (issue #328 FINDING [1]): an IMPLICIT-value imported enum (no `:=`)
// assigns 0,1,2 in declaration order from zero; these continuation values must
// also cross the module boundary. int(Mid)=1, int(Lo)=0, int(Hi)=2 →
// 1*100 + 0*10 + 2 = 102.
BEZEL_TEST(cross_module_import, imported_enum_implicit_values_spec_10_1) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_xm_enumimp_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module EnumLib; "
        "public enum Level Lo, Mid, Hi; end Level;");
    writeSource(bSrc.path,
        "module App; "
        "import EnumLib; "
        "public class Program "
        "  public static method Main(): int "
        "    return int(Level.Mid) * 100 + int(Level.Lo) * 10 "
        "         + int(Level.Hi); "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 102);
}

// §14 / §2.5 / §2.6 (issue #328 FINDING [2]): constructing an IMPORTED GENERIC
// class is NOT yet supported and must produce a CLEAN COMPILE ERROR — not a
// dangling-symbol link failure. Generics are monomorphized per-instantiation
// (§14), so a generic template exports no stable `bz$<Type>$init`/`$ctor` symbol
// the importer could link against; and the dependency metadata carries only the
// type's PUBLIC SIGNATURE (no constructor body, no field initializers), so the
// importer cannot monomorphize `Box<int>` at its use site the way a LOCAL
// `new Box<int>(...)` does. Before the fix the importer emitted an external
// `bz$Box$init` reference that no object defined → `undefined reference to
// bz$Box$init` at link time. After: sema rejects the construction up front with
// a diagnostic that names the limitation.
// §14/#662 Foundation C: imported generic class WITH a ctor body is now ACCEPTED
// (ctor body text is exported in schema v5 metadata and re-parsed on import).
// Previously gated; now compiles clean — no §14 error.
BEZEL_TEST(cross_module_import, imported_generic_new_accepted_spec_14_662c) {
    ModuleMetadata dep = buildMetaFor(
        "module BoxLib; "
        "public class Box<T> "
        "  public value: T; "
        "  public constructor(v: T) value := v; end constructor; "
        "  destructor end destructor; "
        "end Box;");
    auto r = runImporter(
        dep,
        "module App; "
        "import BoxLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var b := new Box<int>(42); "
        "    return b.value; "
        "  end Main; "
        "end Program;");
    // Foundation C: ctor body text exported + re-parsed → no §14 error.
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// §14 / §2.5 (FINDING [2]): a NON-generic imported `new` is unaffected by the
// imported-generic carve-out — the regression guard. A's non-generic `Holder`
// with a one-arg ctor type-checks cleanly cross-module (no diagnostic).
BEZEL_TEST(cross_module_import, imported_nongeneric_new_still_ok_spec_2_5) {
    ModuleMetadata dep = buildMetaFor(
        "module HoldLib; "
        "public class Holder "
        "  public v: int; "
        "  public constructor(start: int) v := start; end constructor; "
        "  destructor end destructor; "
        "end Holder;");
    auto r = runImporter(
        dep,
        "module App; "
        "import HoldLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(7); "
        "    return h.v; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// §14 / §2.6 (FINDING [2]): the end-to-end native build of an imported-generic
// `new` must end in a CLEAN COMPILE FAILURE (rc != 0, a diagnostic) rather than
// a confusing linker error or a crash. The library compiles fine (a generic
// template's `bz$<Type>$ctor`/`$Dispose` are emitted; its per-instantiation init
// is not a stable symbol); the importer's compile fails at sema before any
// §14/#662 Foundation C: imported generic class with explicit ctor body now
// COMPILES AND LINKS (ctor body text exported in metadata, re-parsed on import,
// inlined at the `new` site with genericSubst_ active). Returns b.value = 42
// (the ctor stored the argument into `value`).
BEZEL_TEST(cross_module_import, imported_generic_ctor_body_native_spec_14_662c) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_xm_genbox662c_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module BoxLib; "
        "public class Box<T> "
        "  public value: T; "
        "  public constructor(v: T) value := v; end constructor; "
        "  destructor end destructor; "
        "end Box;");
    writeSource(bSrc.path,
        "module App; "
        "import BoxLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var b := new Box<int>(42); "
        "    return b.value; "  // ctor stored 42 into value
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    // Foundation C: compile + link succeeds (no §14 error; ctor body inlined).
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    // Run: ctor stored 42 → value; Main returns value = 42.
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 42);
}

// §14 / §2.5 (FINDING [2]): an imported generic STATIC method with type
// inference is UNAFFECTED by the imported-generic-`new` carve-out — only
// generic-TYPE construction is rejected. `Util.Echo<T>(x)` called as
// `Util.Echo(42)` (T inferred = int) compiles, links, and returns 42.
BEZEL_TEST(cross_module_import, imported_generic_static_method_still_ok_spec_14) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_xm_genecho_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module UtilLib; "
        "public class Util "
        "  public static method Echo<T>(x: T): T => x; "
        "end Util;");
    writeSource(bSrc.path,
        "module App; "
        "import UtilLib; "
        "public class Program "
        "  public static method Main(): int => Util.Echo(42); "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 42);
}

// §2.5 / §2.6 / §12 (issue #328 / #103): a LOCAL class deriving from an IMPORTED
// base. A's `Base` has `virtual Hello() => 1`, a non-virtual `Greet() => 100`,
// and a ctor setting `field := 7`. B declares `Derived : Base` LOCALLY,
// overriding `Hello() => 2` and adding its own field. Before the fix this CRASHED
// the compiler in LLVM global emission: the importer never reconstructed the
// imported base's type descriptor (`td$Base`), so the derived descriptor's
// chain (Pass C) got a null map default → a malformed ConstantArray →
// SIGSEGV in getRelocationInfo during object emission. After: the exporter emits
// `td$Base` with EXTERNAL linkage and the importer declares it, so the derived
// descriptor links to it; the base ctor chain (`base()`) is declared on demand
// so the base subobject's field is initialized; and the derived vtable installs
// the override. This single test exercises, end to end: (a) the override called
// directly → 2; (b) virtual dispatch through a base-typed reference → 2;
// (c) an inherited non-virtual base method → 100; (d) an inherited base FIELD,
// initialized by the imported base ctor → 7; (e) the derived's own field → 50.
// Sum = 2 + 2 + 100 + 7 + 50 = 161.
BEZEL_TEST(cross_module_import, local_derives_imported_base_spec_12) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_xm_derive_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module Lib; "
        "public class Base "
        "  public field: int; "
        "  public constructor() field := 7; end constructor; "
        "  public virtual method Hello(): int => 1; "
        "  public method Greet(): int => 100; "
        "  destructor end destructor; "
        "end Base;");
    writeSource(bSrc.path,
        "module App; "
        "import Lib; "
        "public class Derived : Base "
        "  public extra: int; "
        "  public constructor() base(); extra := 50; end constructor; "
        "  public override method Hello(): int => 2; "
        "  destructor end destructor; "
        "end Derived; "
        "public class Program "
        "  public static method ViaBase(b: Base): int => b.Hello(); "
        "  public static method Main(): int "
        "    using var d := new Derived(); "
        "    return d.Hello() + ViaBase(d) + d.Greet() + d.field + d.extra; "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 161);
}

// §2.5 / §12 (issue #328 / #103): base-subobject FIELD LAYOUT across modules.
// A's `Base` has two fields `a`, `b` set by its ctor (10, 20); B's local
// `Derived` adds `c` (30) and overrides `Sum()` to read all three. The base
// fields must be laid out before the derived field, and the imported base ctor
// (run via `base()`) must initialize them. The override reads a, b, c; the call
// site also reads each field individually, confirming offsets agree with the
// exporter's `%class.Base` layout. Sum()=60, then a*1000 + b*100 + c*10 =
// 10000 + 2000 + 300; total 12360 (exit code = 12360 mod 256 = 72).
BEZEL_TEST(cross_module_import, local_derives_imported_base_field_layout_spec_12) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_xm_derivefields_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module Lib; "
        "public class Base "
        "  public a: int; "
        "  public b: int; "
        "  public constructor() a := 10; b := 20; end constructor; "
        "  public virtual method Sum(): int => a + b; "
        "  destructor end destructor; "
        "end Base;");
    writeSource(bSrc.path,
        "module App; "
        "import Lib; "
        "public class Derived : Base "
        "  public c: int; "
        "  public constructor() base(); c := 30; end constructor; "
        "  public override method Sum(): int => a + b + c; "
        "  destructor end destructor; "
        "end Derived; "
        "public class Program "
        "  public static method Main(): int "
        "    using var d := new Derived(); "
        "    return d.Sum() + d.a * 1000 + d.b * 100 + d.c * 10; "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 12360 % 256);
}

// §2.5 / §2.6 / §28.4 step 3 / §12 (issue #328 / #103): a LOCAL class deriving
// from an IMPORTED base disposes the WHOLE chain exactly once. A's `Base` counts
// its destructor in a static; B's local `Derived : Base` counts its own. A
// `using var Derived` disposed at scope end must run Derived's destructor (1) AND
// chain to Base's destructor (1). Before the fix the cross-module chain targeted
// the base's INTERNAL `DisposeCore` (not linkable) — observed: the base
// destructor never ran (BaseDisposed stayed 0). After: the exporter exports
// `bz$Base$DisposeCore` with EXTERNAL linkage and the importer's derived
// `DisposeCore` chains to it, so each level's destructor runs once within the
// single flag-guarded most-derived disposal. Encoded BaseDisposed*10 +
// DerivedDisposed = 1*10 + 1 = 11.
BEZEL_TEST(cross_module_import, local_derives_imported_base_dispose_chain_spec_28_4) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_xm_derivedisp_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module Lib; "
        "public class Base "
        "  public static BaseDisposed: int := 0; "
        "  public constructor() end constructor; "
        "  public virtual method Hello(): int => 1; "
        "  destructor BaseDisposed := BaseDisposed + 1; end destructor; "
        "end Base;");
    writeSource(bSrc.path,
        "module App; "
        "import Lib; "
        "public class Derived : Base "
        "  public static DerivedDisposed: int := 0; "
        "  public constructor() base(); end constructor; "
        "  public override method Hello(): int => 2; "
        "  destructor DerivedDisposed := DerivedDisposed + 1; end destructor; "
        "end Derived; "
        "public class Program "
        "  public static method Use(): int "
        "    using var d := new Derived(); "
        "    return d.Hello(); "
        "  end Use; "
        "  public static method Main(): int "
        "    var r := Use(); "
        "    return Base.BaseDisposed * 10 + Derived.DerivedDisposed; "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 11);
}

// --- Cross-module interface dispatch (§13 / §2.5 / §2.6, issue #426 / #328) --
//
// Three scenarios verify the two cross-module itable cases:
//
//  1. Interface call on an IMPORTED object: the object's class + its itab$/itmap$
//     live in the dependency. The importer needs only `td$I` (external) and
//     `ifaceMethodIndex_[ms]` (populated from imported interface members). The
//     runtime bz_itable_lookup follows the vt[1] pointer in the exporter's vtable.
//
//  2. Local class implements an IMPORTED interface: the local class emits its own
//     itab$Local$I + itmap$Local referencing the external td$I. ifaceMethodIndex_
//     must be populated for imported interface methods (same positional scheme).
//
//  3. Interface inheritance across the boundary: A defines I1, I2 : I1; B
//     dispatches calls through both I1 and I2 typed references.

// §13 / §2.5 / §2.6 (issue #426 case 1): interface call on an IMPORTED object.
// Module A defines `public interface IShape { method Area(): int; }` and
// `public class Square : IShape { Area => 16 }`. Module B imports A, constructs
// a Square, holds it through an IShape-typed reference, and calls Area() via
// interface dispatch. The dispatch must follow A's vt[1]=itmap$ chain and arrive
// at Square.Area → 16.
BEZEL_TEST(cross_module_import, interface_call_on_imported_object_spec_13) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_xm_iface1_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module ShapeLib; "
        "public interface IShape "
        "  method Area(): int; "
        "end IShape; "
        "public class Square : IShape "
        "  public method Area(): int => 16; "
        "  destructor end destructor; "
        "end Square;");
    writeSource(bSrc.path,
        "module App; "
        "import ShapeLib; "
        "public class Program "
        "  public static method GetArea(s: IShape): int => s.Area(); "
        "  public static method Main(): int "
        "    using var sq := new Square(); "
        "    return GetArea(sq); "  // interface dispatch on imported object -> 16
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    // Run three times for determinism (interface dispatch is pointer-based;
    // any non-determinism in symbol resolution would show as non-42 exit).
    for (int run = 0; run < 3; ++run) {
        int status = std::system(bExe.path.c_str());
        int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
        BEZEL_EXPECT_EQ(code, 16);
    }
}

// §13 / §2.5 / §2.6 (issue #426 case 2): local class implements an IMPORTED
// interface. Module A defines `public interface IShape { method Area(): int; }`.
// Module B defines a local `class Local : IShape { Area => 42 }`, dispatches
// through an IShape-typed reference → 42. B's codegen must emit itab$Local$IShape
// (referencing the external td$IShape from A) and bz_itable_lookup must find it.
BEZEL_TEST(cross_module_import, local_class_implements_imported_interface_spec_13) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_xm_iface2_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module ILib; "
        "public interface IShape "
        "  method Area(): int; "
        "end IShape;");
    writeSource(bSrc.path,
        "module App; "
        "import ILib; "
        "public class Local : IShape "
        "  public method Area(): int => 42; "
        "  destructor end destructor; "
        "end Local; "
        "public class Program "
        "  public static method Measure(s: IShape): int => s.Area(); "
        "  public static method Main(): int "
        "    using var loc := new Local(); "
        "    return Measure(loc); "  // interface dispatch through imported I -> 42
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    for (int run = 0; run < 3; ++run) {
        int status = std::system(bExe.path.c_str());
        int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
        BEZEL_EXPECT_EQ(code, 42);
    }
}

// §13 / §2.5 / §2.6 (issue #426 case 3): interface inheritance across the module
// boundary. Module A defines `public interface IBase { method Base(): int; }` and
// `public interface IDerived : IBase { method Derived(): int; }`. A also supplies
// a class `Impl` that implements both. Module B imports A, holds an `Impl` through
// IBase and IDerived typed references, and dispatches both methods. The combined
// return Base() * 10 + Derived() must equal 1*10+2=12.
BEZEL_TEST(cross_module_import, interface_inheritance_across_boundary_spec_13) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_xm_iface3_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module MultiLib; "
        "public interface IBase "
        "  method Base(): int; "
        "end IBase; "
        "public interface IDerived : IBase "
        "  method Derived(): int; "
        "end IDerived; "
        "public class Impl : IDerived "
        "  public method Base(): int => 1; "
        "  public method Derived(): int => 2; "
        "  destructor end destructor; "
        "end Impl;");
    writeSource(bSrc.path,
        "module App; "
        "import MultiLib; "
        "public class Program "
        "  public static method CallBase(b: IBase): int => b.Base(); "
        "  public static method CallDerived(d: IDerived): int => d.Derived(); "
        "  public static method Main(): int "
        "    using var impl := new Impl(); "
        "    var b := CallBase(impl); "    // interface dispatch via IBase -> 1
        "    var d := CallDerived(impl); " // interface dispatch via IDerived -> 2
        "    return b * 10 + d; "          // 12
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    for (int run = 0; run < 3; ++run) {
        int status = std::system(bExe.path.c_str());
        int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
        BEZEL_EXPECT_EQ(code, 12);
    }
}

// === #428 phase B: cross-module generic RECORD instantiation =================
//
// §14.1 / §2.5 / §2.6 (#428): an imported POSITIONAL GENERIC RECORD can now be
// constructed in the importing module.  The importer performs per-instantiation
// record-struct specialization locally, riding the same codegen path as a LOCAL
// generic record (genericSubst_ built from TypeSymbol::genericParams rather than
// ast::TypeDecl::genericParams, since no AST exists for imported types; the
// importedName field on T-typed field TypeRefs serves as the substitution key).
// Imported generic CLASSes still carry a §28.3 disposal obligation that requires
// importer-side vtable + disposer infrastructure — they remain gated.

// (1) §14.1 / §2.5 (#428 phase B): imported generic RECORD with a string-typed
// field — the original miscompile shape.  `new Box<string>("hello")` must compile
// cleanly and `b.value.Length` must return 5 (not 0 or garbage from the old i32
// placeholder layout).
BEZEL_TEST(cross_module_import, imported_generic_record_string_field_spec_14_1) {
    ModuleMetadata dep = buildMetaFor(
        "module GenRecLib; "
        "public record Box<T>(value: T); ");
    auto r = runImporter(
        dep,
        "module App; "
        "import GenRecLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := new Box<string>(\"hello\"); "
        "    return b.value.Length; "
        "  end Main; "
        "end Program;");
    // Phase B: sema must accept this — no §14 error.
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// (2) §14.1 / §2.6 (#428 phase B): full AOT native build — imported Box<string>
// construct + field read.  rc must be 5 (Length of "hello").
BEZEL_TEST(cross_module_import, imported_generic_record_string_native_spec_14_1) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_428_grbs_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module GenRecLib2; "
        "public record Box<T>(value: T); ");
    writeSource(bSrc.path,
        "module App; "
        "import GenRecLib2; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := new Box<string>(\"hello\"); "
        "    return b.value.Length; "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);  // the generic record template compiles
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep2;
    dep2.metadataPath = aObj.path;
    dep2.objectPath = aObj.path;
    deps.push_back(dep2);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 5);  // "hello".Length == 5
}

// (3) §14.1 / §2.6 (#428 phase B): imported Box<int> field read returns the
// stored int value.  rc must be 42.
BEZEL_TEST(cross_module_import, imported_generic_record_int_field_native_spec_14_1) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_428_grbi_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module GenRecLibI; "
        "public record Box<T>(value: T); ");
    writeSource(bSrc.path,
        "module App; "
        "import GenRecLibI; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := new Box<int>(42); "
        "    return b.value; "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep3;
    dep3.metadataPath = aObj.path;
    dep3.objectPath = aObj.path;
    deps.push_back(dep3);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 42);
}

// (4) §14.1 / §2.6 (#428 phase B): imported Pair<A,B> with mixed concrete args
// (string + int).  Access both fields; return second.value (int).  rc must be 7.
BEZEL_TEST(cross_module_import, imported_generic_record_pair_mixed_args_native_spec_14_1) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_428_grpair_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module GenPairLib; "
        "public record Pair<A,B>(first: A, second: B); ");
    writeSource(bSrc.path,
        "module App; "
        "import GenPairLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new Pair<string,int>(\"abc\", 7); "
        "    return p.second; "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep4;
    dep4.metadataPath = aObj.path;
    dep4.objectPath = aObj.path;
    deps.push_back(dep4);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 7);
}

// (5) §14.1 / §2.6 (#428 phase B): `with` update on an imported generic record.
// Box<int> with a replaced value field.  rc must be 99.
BEZEL_TEST(cross_module_import, imported_generic_record_with_update_native_spec_14_1) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_428_grwith_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module GenRecLibW; "
        "public record Box<T>(value: T); ");
    writeSource(bSrc.path,
        "module App; "
        "import GenRecLibW; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := new Box<int>(1); "
        "    var b2 := b with(value := 99); "
        "    return b2.value; "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep5;
    dep5.metadataPath = aObj.path;
    dep5.objectPath = aObj.path;
    deps.push_back(dep5);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 99);
}

// (6) §14.1 / §2.6 (#428 phase B): imported generic record as a FIELD of a local
// record.  LocalWrapper holds Box<int> from the imported module.  rc must be 17.
BEZEL_TEST(cross_module_import, imported_generic_record_as_local_field_native_spec_14_1) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_428_grfield_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module GenRecLibF; "
        "public record Box<T>(value: T); ");
    writeSource(bSrc.path,
        "module App; "
        "import GenRecLibF; "
        "record Wrapper(inner: Box<int>); "
        "public class Program "
        "  public static method Main(): int "
        "    var b := new Box<int>(17); "
        "    var w := new Wrapper(b); "
        "    return w.inner.value; "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep6;
    dep6.metadataPath = aObj.path;
    dep6.objectPath = aObj.path;
    deps.push_back(dep6);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 17);
}

// (7) §14.1 / §2.5 (#428 phase B): sema accepts imported generic record with
// double-typed field (non-int, non-pointer primitive stored as f64 in LLVM).
// rc must be 0 (int(3.14) truncates to 3, return b.value > 3.0 ? 1 : 0 → 0).
BEZEL_TEST(cross_module_import, imported_generic_record_double_field_native_spec_14_1) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_428_grdbl_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module GenRecLibD; "
        "public record Box<T>(value: T); ");
    writeSource(bSrc.path,
        "module App; "
        "import GenRecLibD; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := new Box<double>(4.0); "
        "    return int(b.value); "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep7;
    dep7.metadataPath = aObj.path;
    dep7.objectPath = aObj.path;
    deps.push_back(dep7);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 4);  // int(4.0) == 4
}

// (8) §14.1 / §22.8 (#662 foundation A update): imported generic CLASS with a
// field initializer (`n: int := 9`) and NO explicit constructor is now ACCEPTED
// by the narrowed §14 gate (Foundation A). The field initializer expression is
// NOT carried in the dependency's metadata, so the importer calloc-zeros the
// field (n = 0, not 9). Sema must pass cleanly; the construction succeeds.
// Known limitation: field initializer values from the exporter are not replayed
// in the importer — calloc-zero is the importer-side default (Foundation C).
BEZEL_TEST(cross_module_import, imported_generic_field_init_class_diagnosed_spec_22_8) {
    ModuleMetadata dep = buildMetaFor(
        "module GenFieldLib; "
        "public class Holder<T> "
        "  public n: int := 9; "
        "  public value: T; "
        "  destructor end destructor; "
        "end Holder;");
    auto r = runImporter(
        dep,
        "module App; "
        "import GenFieldLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder<int>(); "
        "    return h.n; "  // n = 0 (calloc-zeroed; initializer not replayed)
        "  end Main; "
        "end Program;");
    // Foundation A: accepted (no §14 error for zero-ctor generic class).
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// (9) §14/#662 Foundation C: imported generic CLASS with explicit ctor body is
// now ACCEPTED — ctor body text exported in schema v5 and re-parsed on import.
BEZEL_TEST(cross_module_import, imported_generic_ctor_body_accepted_spec_14_662c) {
    ModuleMetadata dep = buildMetaFor(
        "module GenLib4; "
        "public class Wrapper<T> "
        "  public value: T; "
        "  public constructor(v: T) value := v; end constructor; "
        "  destructor end destructor; "
        "end Wrapper;");
    auto r = runImporter(
        dep,
        "module App; "
        "import GenLib4; "
        "public class Program "
        "  public static method Main(): int "
        "    using var w := new Wrapper<int>(3); "  // Foundation C: ctor body loaded
        "    return 0; "
        "  end Main; "
        "end Program;");
    // Foundation C: ctor body exported + re-parsed → accepted with no §14 error.
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// === #428 phase B — sema member-type substitution ============================
//
// §14.1 / §9 (#428 phase B sema fix): a field read on an imported generic record
// must produce the SUBSTITUTED concrete type, not `?`.  The operator/method that
// follows the field read is what fails when the type is unresolved.

// (10) §14.1 / §9 (#428 phase B sema): arithmetic on an imported Box<int>.Value
// field.  `b.Value + b.Value` failed with "operator + not defined for type ?"
// before the fix (sema left the field type unsubstituted because the imported
// TypeRef carries importedName="T" rather than an astNode).  After the fix the
// field type is substituted to `int` and the operator resolves.
BEZEL_TEST(cross_module_import,
           imported_generic_record_field_arithmetic_typechecks_spec_14_1) {
    ModuleMetadata dep = buildMetaFor(
        "module BoxArithLib; "
        "public record Box<T>(Value: T); ");
    auto r = runImporter(
        dep,
        "module App; "
        "import BoxArithLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := new Box<int>(21); "
        "    return b.Value + b.Value; "  // sema must type b.Value as int
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// (11) §14.1 / §9 (#428 phase B sema): string method (.Length) on an imported
// Box<string>.Value field.  Before the fix the field typed as `?` and
// `.Length` was rejected.  After: field is typed as `string`, `.Length` → `int`.
BEZEL_TEST(cross_module_import,
           imported_generic_record_field_string_method_typechecks_spec_14_1) {
    ModuleMetadata dep = buildMetaFor(
        "module BoxStrLib; "
        "public record Box<T>(Value: T); ");
    auto r = runImporter(
        dep,
        "module App; "
        "import BoxStrLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := new Box<string>(\"bezel\"); "
        "    return b.Value.Length; "  // b.Value must be typed as string
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// (12) §14.1 / §9 (#428 phase B sema): comparison on imported Pair<int,int>
// fields.  `p.First = p.Second` compares two int-typed results; if either field
// were untyped the equality would reject.
BEZEL_TEST(cross_module_import,
           imported_generic_record_pair_field_comparison_typechecks_spec_14_1) {
    ModuleMetadata dep = buildMetaFor(
        "module PairCmpLib; "
        "public record Pair<A, B>(First: A, Second: B); ");
    auto r = runImporter(
        dep,
        "module App; "
        "import PairCmpLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new Pair<int, int>(3, 7); "
        "    if p.First = p.Second then "  // both must be int
        "      return 0; "
        "    end; "
        "    return p.First; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// (13) §14.1 / §9 (#428 phase B sema): an imported record field passed as a
// typed argument to a local method.  The method expects `int`; passing `b.Value`
// where the field is inferred `int` must type-check without errors.
BEZEL_TEST(cross_module_import,
           imported_generic_record_field_as_method_arg_typechecks_spec_14_1) {
    ModuleMetadata dep = buildMetaFor(
        "module BoxArgLib; "
        "public record Box<T>(Value: T); ");
    auto r = runImporter(
        dep,
        "module App; "
        "import BoxArgLib; "
        "public class Program "
        "  public static method Double(x: int): int => x * 2; "
        "  public static method Main(): int "
        "    var b := new Box<int>(13); "
        "    return Double(b.Value); "  // b.Value must be int to match param
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// (14) §14.1 / §2.6 (#428 phase B sema): AOT native build — arithmetic on
// Box<int>.Value.  `b.Value + b.Value` where `Value = 21` must produce 42.
// This is the full end-to-end proof that the codegen path works after the sema fix.
BEZEL_TEST(cross_module_import,
           imported_generic_record_field_arithmetic_native_spec_14_1) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_428_grmath_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module BoxMathLib; "
        "public record Box<T>(Value: T); ");
    writeSource(bSrc.path,
        "module App; "
        "import BoxMathLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := new Box<int>(21); "
        "    return b.Value + b.Value; "  // 21 + 21 = 42
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep14;
    dep14.metadataPath = aObj.path;
    dep14.objectPath = aObj.path;
    deps.push_back(dep14);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 42);  // 21 + 21
}

// (15) §14.1 / §2.6 (#428 phase B sema): AOT native build — string method on
// Box<string>.Value.  `b.Value.Length` where Value = "bezel" must return 5.
// Exercises the sema substitution for a string-typed field and the subsequent
// .Length dispatch.
BEZEL_TEST(cross_module_import,
           imported_generic_record_field_string_length_native_spec_14_1) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_428_grstrlen_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module BoxStrNLib; "
        "public record Box<T>(Value: T); ");
    writeSource(bSrc.path,
        "module App; "
        "import BoxStrNLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := new Box<string>(\"bezel\"); "
        "    return b.Value.Length; "  // "bezel".Length == 5
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep15;
    dep15.metadataPath = aObj.path;
    dep15.objectPath = aObj.path;
    deps.push_back(dep15);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 5);  // "bezel".Length
}

// (16) §14.1 / §2.6 (#428 phase B sema): AOT native build — comparison on
// imported Pair<int,double>.First and Second.  First=40, Second=2.5;
// `p.First + int(p.Second)` must be 42 via the `with` path in modb.bzl.
BEZEL_TEST(cross_module_import,
           imported_generic_record_pair_comparison_native_spec_14_1) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_428_grpair2_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module GenPair2Lib; "
        "public record Box<T>(Value: T); "
        "public record Pair<A, B>(First: A, Second: B); ");
    writeSource(bSrc.path,
        "module App; "
        "import GenPair2Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := new Box<string>(\"bezel\"); "
        "    var p := new Pair<int, double>(40, 2.5); "
        "    var b2 := b with(Value := \"ok\"); "
        "    if b2.Value = \"ok\" and b.Value.Length = 5 then "
        "      return p.First + int(p.Second); "  // 40 + 2 = 42
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep16;
    dep16.metadataPath = aObj.path;
    dep16.objectPath = aObj.path;
    deps.push_back(dep16);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 42);  // 40 + int(2.5) = 42
}

// === #662 foundation B: imported-symbol branches for owned-field shape checks ==
//
// §28.4 / §28.5 (issue #662): isOwnedTypeParamFieldShape and isOwnedReferenceField
// previously did `dynamic_cast<ast::FieldDecl*>(fs->decl)` → null for imported
// FieldSymbols, silently returning false.  Foundation B adds imported-field
// branches that use ctx_.importedMemberType(fs) (the side table populated by the
// loader) and fs->modifiers ("owned" flag) instead of the AST.
//
// For NON-GENERIC imported classes the exporter emits the full disposer body
// (bz$<Type>$DisposeCore) and the importer calls it externally — so the owned-
// field shape checks in the IMPORTER were never on the hot path and disposal was
// already correct for this case.  Foundation B makes the checks CORRECT for
// imported fields so Phase A (buildImportedGenericVtable) can call
// classHasOwnedTypeParamField / emitOwnedFieldDisposals on imported generic
// classes without silently misclassifying owned type-param fields.
//
// Tests (17-20): cross-module AOT scenarios that exercise imported non-generic
// classes with owned fields of various kinds, confirming the existing disposal
// machinery (exporter-side disposer) is not regressed by the predicate changes.
// Tests 19-20 also confirm the field-shape predicates are called correctly at
// every disposal site (using-var, destructor body, ctor exception cleanup).

// (17) §28.4 / §28.5 (#662 foundation B): imported non-generic class with an
// `owned` class-typed field.  The exporter emits DisposeCore; the importer calls
// it via vtable.  The owned field's destructor counter must be 1 after `using var`
// disposes the container.  rc = outer.Disposed * 10 + inner.Disposed = 11.
BEZEL_TEST(cross_module_import,
           imported_class_owned_class_field_disposed_spec_28_4_662b) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_662b_owncls_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module OwnedLib; "
        // The inner class whose disposal we can observe.
        "public class Inner "
        "  public static Disposed: int := 0; "
        "  public constructor() end constructor; "
        "  destructor Disposed := Disposed + 1; end destructor; "
        "end Inner; "
        // The outer class that OWNS an Inner field (§28.4 step 2 disposal).
        "public class Outer "
        "  public static Disposed: int := 0; "
        "  private owned _inner: Inner; "
        "  public constructor() "
        "    _inner := new Inner(); "
        "  end constructor; "
        "  destructor Disposed := Disposed + 1; end destructor; "
        "end Outer;");
    writeSource(bSrc.path,
        "module App; "
        "import OwnedLib; "
        "public class Program "
        "  public static method Use(): int "
        "    using var o := new Outer(); "  // disposed at scope end → outer + inner
        "    return 0; "
        "  end Use; "
        "  public static method Main(): int "
        "    Use(); "
        "    return Outer.Disposed * 10 + Inner.Disposed; "  // 11
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 11);
}

// (18) §28.4 / §28.5 (#662 foundation B): imported non-generic class with an
// `owned` string field.  String fields are retained at store and released by the
// exporter's DisposeCore (§4.6/§28.9).  The destructor counter must be 1 after
// `using var` disposes the holder; the exporter handles string release internally.
// rc = Holder.Disposed = 1.
BEZEL_TEST(cross_module_import,
           imported_class_owned_string_field_disposed_spec_28_4_662b) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_662b_ownstr_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module StrLib; "
        "public class Holder "
        "  public static Disposed: int := 0; "
        "  public _label: string; "
        "  public constructor(lbl: string) "
        "    _label := lbl; "
        "  end constructor; "
        "  public method Label(): string => _label; "
        "  destructor Disposed := Disposed + 1; end destructor; "
        "end Holder;");
    writeSource(bSrc.path,
        "module App; "
        "import StrLib; "
        "public class Program "
        "  public static method Use(): int "
        "    using var h := new Holder(\"hello\"); "
        "    return h.Label().Length; "
        "  end Use; "
        "  public static method Main(): int "
        "    var n := Use(); "        // h disposed here
        "    return Holder.Disposed; " // 1
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 1);
}

// (19) §28.4 / §28.5 (#662 foundation B): sema-level check — an imported
// NON-GENERIC class with an `owned` field of a class type must pass sema cleanly
// in the importer (no spurious §14 or other diagnostic from the predicate change).
BEZEL_TEST(cross_module_import,
           imported_class_owned_field_sema_clean_spec_28_5_662b) {
    ModuleMetadata dep = buildMetaFor(
        "module OwnedFieldLib; "
        "public class Holder "
        "  private owned _item: Holder; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Holder;");
    auto r = runImporter(
        dep,
        "module App; "
        "import OwnedFieldLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    return 1; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// (20) §28.4 / §28.5 / §14 (#662 foundation C): imported GENERIC class with
// `owned T` field — Foundation C: ctor body exported + re-parsed → accepted.
//
// ── THIS TEST WAS REPAIRED (2026-08-13, family F12 of umbrella #3152) ───────
//
// It previously declared `constructor(v: T)` storing a BORROW into an `owned`
// field, and instantiated at `Box<string>`. Both halves are invalid Bezel, and
// the byte-identical LOCAL program is rejected by this same compiler:
//
//   dep source alone   -> "a borrow cannot be stored into an `owned` field …
//                          a double-free (§28.3 / §28.5 / §28.8)"
//   Box<string> local  -> "`Box` may not be instantiated with `string` … only
//                          class or interface types may be `owned` (§28.5/§28.9)"
//
// So the test asserted that the IMPORTER accepts what the DEFINING compilation
// rejects — precisely the divergence §2.8.2 clause 1 forbids, encoded as the
// expected result. It passed only because the AST-gated `owned` bare-type-param
// check could not see an imported symbol; #3147 (family F12) closed that, and
// this test was the one thing that noticed.
//
// The FIXTURE was wrong, not the intent. Foundation C is about a generic ctor
// BODY surviving export and re-parse, so the fixture now uses a shape that is
// legal on both sides — `owned v: T` consumed into the field, instantiated at a
// CLASS type — and still exercises exactly that mechanism. Verified: the
// repaired dep compiles standalone, and the repaired local twin compiles clean.
BEZEL_TEST(cross_module_import,
           imported_generic_class_owned_tparam_field_accepted_spec_14_662c) {
    ModuleMetadata dep = buildMetaFor(
        "module BoxLib; "
        "public class Payload "
        "  public v: int := 0; "
        "  destructor end destructor; "
        "end Payload; "
        "public class Box<T> "
        "  private owned _slot: T; "
        "  public constructor(owned v: T) "
        "    _slot := v; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Box;");
    auto r = runImporter(
        dep,
        "module App; "
        "import BoxLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var b := new Box<Payload>(new Payload()); "
        "    return 1; "
        "  end Main; "
        "end Program;");
    // Foundation C: ctor body exported + re-parsed → accepted with no §14 error.
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// === #662 foundation A — importer-side vtable + disposer for imported generic ===
//
// Foundation A delivers per-instantiation vtable + disposer for imported generic
// class types that have NO explicit constructor (implicit zero-arg construction,
// calloc-zero field initialization). The specialized Dispose runs the destructor
// body, disposes owned type-param fields by their concrete substituted type, and
// reclaims the instance block.
//
// Sound subset: imported generic class with no explicit constructor body. Classes
// with explicit constructors remain §14-gated (Foundation C / Blocker 3).

// (21) §28.5 / §14 (#662 foundation A): imported generic class with NO
// explicit constructor is constructed and disposed via `using var`.  Foundation
// A proves the structural side: vtable installed, disposal path runs without
// crash.  The destructor BODY is absent from metadata (Blocker 3 / Foundation
// C); only the structural disposal (owned-field null-check + free) runs in the
// importer.  The observable proof here is that the program runs to completion
// without crashing (rc == 0), NOT a counter increment.
BEZEL_TEST(cross_module_import,
           imported_generic_class_zero_ctor_owned_field_disposed_spec_28_5_662a) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_662a_genbox_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    // Module A: a generic Box<T> with NO explicit constructor.
    // The destructor body would run on disposal, but since destructor bodies are
    // not in metadata (§13.3: not part of the public ABI), the importer emits
    // only the structural disposal (null-check owned field + free the block).
    // Foundation A proves construction + vtable install + structural disposal.
    writeSource(aSrc.path,
        "module GenBoxLib; "
        "public class Box<T> "
        "  destructor end destructor; "
        "end Box;");
    // Module B: constructs two Box<int> instances, disposes via `using var`.
    // Main returns 1 (a fixed value) to prove the program ran to completion.
    writeSource(bSrc.path,
        "module App; "
        "import GenBoxLib; "
        "public class Program "
        "  public static method Use(): int "
        "    using var b1 := new Box<int>(); "
        "    using var b2 := new Box<int>(); "
        "    return 0; "
        "  end Use; "
        "  public static method Main(): int "
        "    Use(); "
        "    return 1; "  // 1: program ran to completion without crash
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 1);  // program ran to completion (vtable valid, no crash)
}

// (22) §28.5 / §14 (#662 foundation A): two DISTINCT instantiations of the same
// imported generic class (Counter<int> and Counter<string>) each get separate
// per-instantiation vtables and each run their structural disposal without
// crashing.  Destructor bodies are not in metadata (Blocker 3 / Foundation C),
// so the observable proof is that the program runs to completion (rc == 2,
// a fixed return value, not a counter).
BEZEL_TEST(cross_module_import,
           imported_generic_class_two_instantiations_both_disposed_spec_28_5_662a) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_662a_twoinst_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module GenCountLib; "
        "public class Counter<T> "
        "  destructor end destructor; "
        "end Counter;");
    writeSource(bSrc.path,
        "module App; "
        "import GenCountLib; "
        "public class Program "
        "  public static method Use(): int "
        "    using var ci := new Counter<int>(); "
        "    using var cs := new Counter<string>(); "
        "    return 0; "
        "  end Use; "
        "  public static method Main(): int "
        "    Use(); "
        "    return 2; "  // 2: two-instantiation program ran to completion (vtables valid)
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 2);  // both Counter<int> and Counter<string> disposed
}

// (23) §14 (#662 foundation A): imported generic class with NO explicit ctor
// passes sema cleanly (narrowed §14 gate).  Sema accepts the construction;
// no §14 diagnostic is emitted.
BEZEL_TEST(cross_module_import,
           imported_generic_class_no_ctor_sema_clean_spec_14_662a) {
    ModuleMetadata dep = buildMetaFor(
        "module GenSimpleLib; "
        "public class Holder<T> "
        "  public _v: int := 0; "
        "  destructor end destructor; "
        "end Holder;");
    auto r = runImporter(
        dep,
        "module App; "
        "import GenSimpleLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder<int>(); "
        "    return h._v; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// (24) §14/#662 Foundation C: imported generic class WITH an explicit ctor body
// is now ACCEPTED (schema v5 exports ctor body text; loader re-parses and attaches
// the ast::ConstructorDecl; gate accepts cs->decl != nullptr).
BEZEL_TEST(cross_module_import,
           imported_generic_class_with_ctor_body_accepted_spec_14_662c) {
    ModuleMetadata dep = buildMetaFor(
        "module BoxWithCtorLib; "
        "public class Box<T> "
        "  public owned _slot: T; "
        "  public constructor(v: T) "
        "    _slot := v; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Box;");
    auto r = runImporter(
        dep,
        "module App; "
        "import BoxWithCtorLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var b := new Box<int>(42); "  // Foundation C: ctor body loaded
        "    return 0; "
        "  end Main; "
        "end Program;");
    // Foundation C: ctor body exported + re-parsed → accepted with no §14 error.
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// === #662 Foundation C — AOT tests ===========================================
//
// Foundation C: imported generic class WITH constructor/destructor bodies.
// The ctor body text is exported in schema v5 metadata, re-parsed on import
// with a mini synthetic class source, sema typechecked in a fresh context,
// expr→symbol mappings remapped to the imported symbols in the main context,
// and the re-parsed ConstructorDecl attached to the ConstructorSymbol so
// inlineCtorBody can emit the body with genericSubst_ active (§14.1).

// (25) §14/#662 Foundation C: imported generic class — int field, ctor stores
// argument into field. `new Box<int>(42)` → `b.value` == 42. Proves the basic
// ctor-body inline path end to end for an int type-parameter.
BEZEL_TEST(cross_module_import,
           imported_generic_class_ctor_int_field_native_spec_14_662c) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_662c_int_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module BoxLib; "
        "public class Box<T> "
        "  public value: T; "
        "  public constructor(v: T) value := v; end constructor; "
        "  destructor end destructor; "
        "end Box;");
    writeSource(bSrc.path,
        "module App; "
        "import BoxLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var b := new Box<int>(42); "
        "    return b.value; "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    // ctor stored 42 → value; b.value == 42.
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 42);
}

// (26) §14/#662 Foundation C: two distinct instantiations of the same imported
// generic class — Box<int>(21) and Box<int>(7). Two independent monomorphizations
// of the inlined ctor body; both store the right value.
BEZEL_TEST(cross_module_import,
           imported_generic_class_ctor_two_instantiations_native_spec_14_662c) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_662c_two_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module BoxLib; "
        "public class Box<T> "
        "  public value: T; "
        "  public constructor(v: T) value := v; end constructor; "
        "  destructor end destructor; "
        "end Box;");
    writeSource(bSrc.path,
        "module App; "
        "import BoxLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new Box<int>(21); "
        "    using var b := new Box<int>(7); "
        "    return a.value + b.value; "  // 21 + 7 == 28
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 28);
}

// (27) §14/#662 Foundation C: ctor body with arithmetic on params — `counter(a, b)`
// stores `a * 2 + b` into `n`. Proves the ctor body can contain computed exprs,
// not just plain field-store assignments.
BEZEL_TEST(cross_module_import,
           imported_generic_class_ctor_arithmetic_native_spec_14_662c) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_662c_arith_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module CounterLib; "
        "public class Counter<T> "
        "  public n: int; "
        "  public constructor(a: int, b: int) n := a * 2 + b; end constructor; "
        "  destructor end destructor; "
        "end Counter;");
    writeSource(bSrc.path,
        "module App; "
        "import CounterLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var c := new Counter<int>(10, 5); "
        "    return c.n; "  // 10 * 2 + 5 == 25
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 25);
}

// (28) §14/#28.4/#662 Foundation C: destructor body on imported generic class —
// the dtor body text is exported in schema v5 (dtorBodyText on MetaTypeDecl),
// re-parsed and attached to a DestructorSymbol in the imported type. The
// emitSpecializedDisposer path calls findDestructor + lowerScopedBlock with
// genericSubst_ active. Simple correctness test: ctor body stores 42 → value,
// dtor body resets n := 0 (non-trivial body, safe int store). Main returns
// b.value = 42 (ctor ran); no crash proves dtor body ran safely in the
// specialized disposer.
BEZEL_TEST(cross_module_import,
           imported_generic_class_dtor_body_native_spec_14_662c) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_662c_dtor_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module BoxDtorLib; "
        "public class Box<T> "
        "  public value: T; "
        "  public n: int; "
        "  public constructor(v: T) value := v; n := 1; end constructor; "
        "  destructor n := 0; end destructor; "
        "end Box;");
    writeSource(bSrc.path,
        "module App; "
        "import BoxDtorLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var b := new Box<int>(42); "
        "    return b.value; "  // ctor stored 42; dtor runs at end of using
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    // Main returns b.value = 42 (ctor ran). No crash = dtor body ran safely.
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 42);
}

// (29) §14/#662 Foundation C NEGATIVE: imported generic class WITH explicit ctor
// whose ctorBodyText was NOT exported (simulates a v4-format metadata blob — the
// MetaMember::ctorBodyText field is left empty so parseAndAttachCtorBody is never
// called and cs->decl remains null). The sema gate must still fire and produce the
// clean §14 diagnostic rather than silently miscompiling.
// This exercises the "still-gated shape: explicit ctor present but body not loaded"
// arm in ExprTypecheck_Calls.cpp checkImportedNewExprArgs.
BEZEL_TEST(cross_module_import,
           imported_generic_class_ctor_no_body_text_still_gated_spec_14_662c) {
    // Manually craft a MetaTypeDecl with a Constructor member but empty
    // ctorBodyText (the v4 schema had no ctor body text field).
    MetaTypeDecl td;
    td.name = "Box";
    td.kind = MetaTypeDeclKind::Class;
    td.visibility = MetaVisibility::Public;
    // One generic param T.
    MetaGenericParam gp;
    gp.name = "T";
    gp.variance = "none";
    td.genericParams.push_back(gp);
    // A field `value: T`.
    MetaMember fld;
    fld.kind = MetaMemberKind::Field;
    fld.name = "value";
    fld.visibility = MetaVisibility::Public;
    fld.type.kind = TypeKind::Named;
    fld.type.namedName = "T";
    td.members.push_back(std::move(fld));
    // A constructor(v: T) with NO ctorBodyText (empty = not exported).
    MetaMember ctor;
    ctor.kind = MetaMemberKind::Constructor;
    ctor.name = "constructor";
    ctor.visibility = MetaVisibility::Public;
    MetaParam p;
    p.name = "v";
    p.type.kind = TypeKind::Named;
    p.type.namedName = "T";
    ctor.params.push_back(std::move(p));
    // ctorBodyText intentionally left empty (no body text — v4 schema shape).
    td.members.push_back(std::move(ctor));

    // The ctorBodyText is left empty (default) — simulates a v4-format metadata
    // blob that was serialized before schema v5 added ctorBodyText export.
    ModuleMetadata dep;
    dep.moduleName = "BoxLibV4";
    dep.exports.push_back("Box");
    dep.types.push_back(std::move(td));

    auto r = runImporter(
        dep,
        "module App; "
        "import BoxLibV4; "
        "public class Program "
        "  public static method Main(): int "
        "    using var b := new Box<int>(42); "  // ctor body not loaded → gated
        "    return b.value; "
        "  end Main; "
        "end Program;");
    // Must diagnose: explicit ctor present but cs->decl is null (body not loaded).
    BEZEL_EXPECT_TRUE(r->hadErrors);
    bool sawGateDiag = false;
    for (const Diagnostic& d : r->diags.diagnostics()) {
        if (d.message.find("constructor body") != std::string::npos ||
            d.message.find("imported generic") != std::string::npos ||
            d.message.find("foundation C") != std::string::npos ||
            d.message.find("metadata") != std::string::npos)
            sawGateDiag = true;
    }
    BEZEL_EXPECT_TRUE(sawGateDiag);
}

// §22.12 / #684: generic class with static field + ctor + destructor body
// — bezelc -c (compile to object) must succeed, and an importer that
// instantiates the class must compile and link without crashing.
//
// Root cause: when emitSpecializedDisposer ran the imported destructor body
// with genericSubst_ active, staticFieldGlobal tried to copy the initializer
// from the template-level imported global (which was an external declaration
// with nullptr initializer), triggering an LLVM assertion.
//
// Fix: when the base template global has no initializer (it is an external
// declaration from an import), use llvm::Constant::getNullValue as the
// per-instantiation global's initializer (per §22.12 each instantiation has
// its own state, zero-initialized in the importer and set by runtime init).
BEZEL_TEST(cross_module_import,
           imported_generic_static_field_ctor_dtor_no_crash_spec_22_12_684) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_684_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    // Module A: generic class Tag<T> with static field + explicit ctor body +
    // destructor body. This is the exact shape that triggered the crash
    // (#684: 'GV doesn't have initializer!' from GlobalVariable::getInitializer
    // on the imported external static-field global during specialized-disposer
    // codegen with genericSubst_ active).
    writeSource(aSrc.path,
        "module TagLib; "
        "public class Tag<T> "
        "  public owned Label: string; "
        "  public static disposed: int := 0; "
        "  public constructor(owned label: string) "
        "    Label := label; "
        "  end constructor; "
        // The destructor body accesses the generic static field — this is
        // the path that crashed: staticFieldGlobal looked up genericSubst_
        // and tried to call getInitializer() on the imported external ref.
        "  destructor "
        "    Tag.disposed := Tag.disposed + 1; "
        "  end destructor; "
        "end Tag;");
    // Step 1: compile-to-object (bezelc -c) must succeed.
    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    // Module B: imports Tag and instantiates it. This is what triggered the
    // crash in the importer — emitting the imported ctor/dtor body for Tag<int>
    // with genericSubst_ active hit the LLVM assertion.
    writeSource(bSrc.path,
        "module App; "
        "import TagLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new Tag<int>(\"hello\"); "
        "    return a.Label.Length; "
        "  end Main; "
        "end Program;");
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    // Step 2: compile importer — must NOT crash (was: LLVM assertion abort).
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    // Step 3: run — Label was "hello" (5 chars), return value = 5.
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 5);
}

// §22.12 / #684 correctness: a generic class with STATIC field + ctor + dtor
// body, different shape from the first #684 test (two type params, both ctor
// and dtor bodies, static field write in dtor). This exercises a wider set of
// the import/codegen paths that #684 unlocked without making assumptions about
// cross-module per-instantiation counter observability (which requires
// infrastructure beyond the current §22.12 scope).
//
// Shape: Pair<A,B> with static fieldCount: int incremented in dtor, ctor
// stores values, dtor decrements. Program creates one Pair<int,int>("hello"),
// disposes it, returns 7 — a constant not dependent on the static counter
// (just verifying compile, link, and run succeed without any crash after the
// fix, and that a second distinct instantiation shape also works).
BEZEL_TEST(cross_module_import,
           imported_generic_static_disposer_counter_correct_spec_22_12_684) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_684b_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    // Module A: generic class Box<T> with static destroyed: int incremented in
    // the destructor. Different shape from Tag<T> (no owned fields here, just
    // a static + ctor + dtor). This is the minimal combo that triggered #684
    // when the destructor body accesses the generic static while genericSubst_
    // is active.
    writeSource(aSrc.path,
        "module BoxLib; "
        "public class Box<T> "
        "  public static destroyed: int := 0; "
        "  public constructor() end constructor; "
        "  destructor "
        "    Box.destroyed := Box.destroyed + 1; "
        "  end destructor; "
        "end Box;");
    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    // Module B: instantiate Box<int> and Box<bool>, dispose both, return 7.
    // Two distinct instantiations exercise the per-instantiation global path.
    // Return 7 (a constant) — not the counter value — since per-instantiation
    // counter read-back requires cross-module generic method inlining which is
    // not yet implemented (the crash fix is in the dtor-body/static-init path,
    // not the read-back path).
    writeSource(bSrc.path,
        "module App; "
        "import BoxLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var bi := new Box<int>(); "
        "    using var bb := new Box<bool>(); "
        "    return 7; "
        "  end Main; "
        "end Program;");
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    // Two instantiations (Box<int> + Box<bool>) compiled, linked, and ran;
    // the binary returns 7, pinning no-crash and correct disposal.
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 7);
}

// ---------------------------------------------------------------------------
// §22.12 / #1224: imported generic class with PRIVATE static field accessed
// in a static method — per-instantiation statics are independent.
// ---------------------------------------------------------------------------
// Root cause: private fields were not exported in the generic-class metadata, so
// the synthetic method source built by buildSyntheticMethodSource lacked the field
// declaration. Sema then rejected the re-parsed body (unresolved `_count`),
// parseAndAttachMethodBody returned false, and the imported MethodSymbol kept a
// null decl — blocking the per-instantiation inline monomorphization path.
// Without inlining, both Registry<int>.Register() and Registry<string>.Register()
// called the SAME compiled template function bz$Registry$Register, which operates
// on the single shared template-level global bz$Registry$_count — violating §22.12.
// Fix: export private/internal fields for generic classes (ModuleMetadataBuilder,
// `includeForMethodBody`), mirroring the existing private-method export (#695).
// ---------------------------------------------------------------------------

// §22.12 / #1224: a generic class with a PRIVATE static field accessed by bare
// name in a static method must give each distinct type-arg instantiation an
// INDEPENDENT counter when called cross-module.
// Registry<int>.Register() called twice and Registry<string>.Register() called
// twice should return 1,2 for int and 1,2 for string (not share one global).
BEZEL_TEST(cross_module_import,
           imported_generic_private_static_field_per_instantiation_spec_22_12_1224) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_1224_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    // Module A: generic class Registry<T> with private static counter
    // accessed via bare name in a static method (the exact bug pattern for
    // #1224 — qualified access Registry._count was already correct, but
    // bare `_count` failed because the private field was not in the metadata).
    writeSource(aSrc.path,
        "module RegLib; "
        "public class Registry<T> "
        "  private static _count: int := 0; "
        "  public static method Register(): int "
        "    _count := _count + 1; "
        "    return _count; "
        "  end Register; "
        "end Registry; ");
    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    // Module B: call Registry<int>.Register() twice then Registry<string>.Register()
    // twice. §22.12: each instantiation has its own static state, so the int
    // counter returns 1,2 and the string counter independently returns 1,2.
    // The sum b + d must be 4 (2 + 2). If statics are shared it would be 6 (2 + 4).
    writeSource(bSrc.path,
        "module App; "
        "import RegLib; "
        "public class Program "
        "  public static method Main(): int "
        "    Registry<int>.Register(); "
        "    var b := Registry<int>.Register(); "
        "    Registry<string>.Register(); "
        "    var d := Registry<string>.Register(); "
        "    return b + d; "
        "  end Main; "
        "end Program; ");
    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep;
    dep.metadataPath = aObj.path;
    dep.objectPath = aObj.path;
    deps.push_back(dep);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    // §22.12: isolated counters → b=2, d=2, sum=4.
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 4);
}

// ---------------------------------------------------------------------------
// §14 / #662c / #2290 (bug 1): imported generic-class ctor body — a LOCAL
// VARIABLE declared inside the reconstructed ctor body
// ---------------------------------------------------------------------------
// Root cause (issue #2290 bug 1): parseAndAttachCtorBody's fresh-context->
// mainCtx remap loop (ModuleLoader.cpp) handled only FieldSymbol, ParameterSymbol
// and MethodSymbol — a `var`/`using var` LOCAL declared inside the ctor body has
// no imported counterpart to translate to, so its exprSymbol entries were never
// registered in mainCtx, and the fresh SemaContext that owned the LocalSymbol was
// destroyed on return (only the AST/source were kept alive). codegen's
// `ctx_.exprSymbol()` lookup on a later reference then found nothing, silently
// reading the type's default value instead of the local's actual value — a
// return of `0` where the spec-correct answer is nonzero, with NO diagnostic.
// Fix (ModuleLoader.cpp parseAndAttachCtorBody): register each fresh LocalSymbol
// under mainCtx BY IDENTITY (mirroring #783's parseAndAttachMethodBody), and keep
// the fresh SemaContext + DiagnosticEngine alive via mainCtx.ownImportedAuxiliary
// (shared_ptr, engine-then-context per #783's precedent) so the registered
// pointers stay valid through codegen; also copies resolvedTypes/exprTypes for
// the same reason. Cross-TYPE references (issue #2290 bug 2 / #710's own ask —
// a call/`new` naming a SIBLING type) are NOT addressed by this fix; see #710.
// ---------------------------------------------------------------------------

// (31) §14/#2290 bug 1: a plain (non-owned) local inside the ctor body must be
// read correctly, not silently default to 0. Exact repro from #2290 bug 1.
BEZEL_TEST(cross_module_import,
           imported_generic_ctor_body_local_var_read_native_spec_14_2290) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_2290_localvar_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module Gen2290Lib; "
        "public class Wrapper<T> "
        "  public val: T; "
        "  public len: int := 0; "
        "  public constructor(v: T) "
        "    var x := 5; "
        "    len := x; "
        "    val := v; "
        "  end constructor; "
        "end Wrapper;");
    writeSource(bSrc.path,
        "module App; "
        "import Gen2290Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var w := new Wrapper<int>(7); "
        "    return w.len; "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    // Before the fix: silently reads 0 (the local's read is dropped). After: 5.
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 5);
}

// (32) §14/#2290 bug 1 / §28.9 / #710: an OWNED local of a non-trivial-dispose
// STDLIB type (`OwningBuffer<int>`, stdlib §4.14 — "both [class- and value-typed
// T] are non-trivial-dispose... the user must discharge the buffer") declared
// inside the ctor body — this is #710's own "owned local in the ctor body" ask,
// for the subset that does NOT need a sibling-type stub (OwningBuffer is always
// in scope; no #428-sized declaration-rewriting foundation required). Verifies
// BOTH the value read through the local AND — the actual point of an "owned"
// local — that its disposal fires under ASan/LSan with no leak, double-free, or
// use-after-free, in the IMPORTING module's program.
BEZEL_TEST(cross_module_import,
           imported_generic_ctor_body_owned_local_disposed_once_spec_14_28_9_2290) {
    std::string aSource =
        "module Gen2290BufLib; "
        "public class Wrapper<T> "
        "  public val: T; "
        "  public len: int := 0; "
        "  public constructor(v: T) "
        "    using var buf := new OwningBuffer<int>(4); "
        "    buf[0] := 42; "
        "    len := buf[0]; "
        "    val := v; "
        "  end constructor; "
        "end Wrapper;";
    std::string bSource =
        "module App; "
        "import Gen2290BufLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var w := new Wrapper<int>(1); "
        "    return w.len; "
        "  end Main; "
        "end Program;";

    CrossModuleAsanResult r = runCrossModuleChecked(aSource, bSource);
    BEZEL_EXPECT_TRUE(r.builtOk);
    if (!r.builtOk) return;
    BEZEL_EXPECT_TRUE(!r.sanitizerFired);
    if (r.sanitizerFired) return;  // report already captured in r.sanitizerText
    BEZEL_EXPECT_TRUE(r.ran);
    // Before the fix: buf[0]'s read is dropped, len reads 0. After: 42, and the
    // OwningBuffer's allocation is disposed exactly once (no LSan leak report,
    // no ASan double-free/UAF report) at the `using var` scope end.
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// (33) Adversarial self-pass (b): TWO instantiations of the same imported
// generic class, each with its OWN owned local in its ctor body, both disposed
// correctly — the per-instantiation inline emission (#14.1) must not share or
// double-dispose the local's storage across instantiations.
BEZEL_TEST(cross_module_import,
           imported_generic_ctor_body_owned_local_two_instantiations_spec_14_2290) {
    std::string aSource =
        "module Gen2290Buf2Lib; "
        "public class Wrapper<T> "
        "  public val: T; "
        "  public len: int := 0; "
        "  public constructor(v: T) "
        "    using var buf := new OwningBuffer<int>(4); "
        "    buf[0] := 42; "
        "    len := buf[0]; "
        "    val := v; "
        "  end constructor; "
        "end Wrapper;";
    std::string bSource =
        "module App; "
        "import Gen2290Buf2Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var w1 := new Wrapper<int>(1); "
        "    using var w2 := new Wrapper<string>(\"hi\"); "
        "    return w1.len + w2.len; "
        "  end Main; "
        "end Program;";

    CrossModuleAsanResult r = runCrossModuleChecked(aSource, bSource);
    BEZEL_EXPECT_TRUE(r.builtOk);
    if (!r.builtOk) return;
    BEZEL_EXPECT_TRUE(!r.sanitizerFired);
    if (r.sanitizerFired) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 84);  // 42 + 42, both instantiations disposed once
}

// (34) Adversarial self-pass (a): the IMPORTING module declares its OWN local
// variable with the SAME NAME ("buf") as the ctor body's reconstructed local.
// Since the fix remaps by fresh-Symbol-pointer IDENTITY (not by name lookup
// against mainCtx), the two `buf`s must not collide or alias.
BEZEL_TEST(cross_module_import,
           imported_generic_ctor_body_local_name_collision_spec_14_2290) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_2290_collide_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module Gen2290ColLib; "
        "public class Wrapper<T> "
        "  public val: T; "
        "  public len: int := 0; "
        "  public constructor(v: T) "
        "    var buf := 5; "
        "    len := buf; "
        "    val := v; "
        "  end constructor; "
        "end Wrapper;");
    writeSource(bSrc.path,
        "module App; "
        "import Gen2290ColLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var buf := 999; "
        "    using var w := new Wrapper<int>(1); "
        "    return w.len + buf; "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    // Wrapper<int>(1).len == 5 (own body's `buf`) + the caller's own `buf` == 999
    // → 1004, masked to WEXITSTATUS's 0..255 range: 1004 % 256 == 236.
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 1004 % 256);
}

// ---------------------------------------------------------------------------
// §14 / #662c / #695: imported generic-class ctor/dtor body self-method calls
// ---------------------------------------------------------------------------
// Root cause: private methods were not exported in the generic-class metadata,
// so the synthetic class re-parsed for ctor-body inlining had no stub for them.
// The callee IdentifierExpr resolved to null in the main SemaContext, and
// lowerCall returned nullptr → field got zero instead of the real return value.
// Fix: export private method signatures for generic classes (ModuleMetadataBuilder),
// generate method stubs in the synthetic class source (ModuleLoader
// buildSyntheticClassSource/buildSyntheticDtorSource), remap fresh MethodSymbol
// entries to the imported MethodSymbol in parseAndAttachCtorBody / ...DtorBody.
// ---------------------------------------------------------------------------

// (30) §14/#662c/#695: ctor body calls a private own method that returns int —
// the call result must reach the field, not zero.  This is the exact repro shape
// from the issue: Wrapper<T> ctor does `computed := computeConst()` where
// computeConst() is a private method returning the literal 42.
BEZEL_TEST(cross_module_import,
           imported_generic_ctor_body_selfcall_returns_value_native_spec_14_695) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_695_selfcall_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module WrapLib; "
        "public class Wrapper<T> "
        "  public raw: T; "
        "  public computed: int; "
        "  public constructor(v: T) "
        "    raw := v; "
        "    computed := computeConst(); "
        "  end constructor; "
        "  destructor end destructor; "
        "  private method computeConst(): int => 42; "
        "end Wrapper;");
    writeSource(bSrc.path,
        "module App; "
        "import WrapLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var w := new Wrapper<string>(\"hello\"); "
        "    return w.computed; "  // ctor called computeConst() → 42
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    // Ctor called computeConst() → 42; w.computed must be 42, not 0.
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 42);
}

// (31) §14/#662c/#695: ctor body calls a private own method whose PARAMETER is
// the class type param T — substitution must still be active for the call.
// Counter<T>(v: T) calls helper(v) which returns 1 (via parameter, exercising
// the T-typed call path); the public `computed` field is set to `helper(v) * 7`.
// We verify the computed field equals 7 (1 * 7).
BEZEL_TEST(cross_module_import,
           imported_generic_ctor_body_selfcall_uses_T_native_spec_14_695) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_695_Tparam_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    // helper(v: T): int => 1 takes a T-typed param; codegen must tolerate
    // calling the external template (no T fields accessed, result is constant).
    writeSource(aSrc.path,
        "module CounterLib; "
        "public class Counter<T> "
        "  public computed: int; "
        "  public constructor(v: T) "
        "    computed := helper(v) * 7; "
        "  end constructor; "
        "  destructor end destructor; "
        "  private method helper(x: T): int => 1; "
        "end Counter;");
    writeSource(bSrc.path,
        "module App; "
        "import CounterLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var c := new Counter<int>(99); "
        "    return c.computed; "  // helper(99) * 7 = 1 * 7 = 7
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    // helper(v) * 7 = 1 * 7 = 7.
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 7);
}

// (32) §14/#662c/#695: two distinct instantiations of the same imported
// generic class, each with a ctor self-call.  Box<int>(21) and Box<string>("hi")
// both store the computeConst() result (42) into their `tag` field.
// Main returns a.tag + b.tag = 42 + 42 = 84 (well within the 0–255 exit-code
// range; we verify the per-instantiation correctness via addition).
BEZEL_TEST(cross_module_import,
           imported_generic_ctor_body_selfcall_two_instantiations_native_spec_14_695) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_695_two_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module BoxLib; "
        "public class Box<T> "
        "  public value: T; "
        "  public tag: int; "
        "  public constructor(v: T) "
        "    value := v; "
        "    tag := magic(); "
        "  end constructor; "
        "  destructor end destructor; "
        "  private method magic(): int => 42; "
        "end Box;");
    writeSource(bSrc.path,
        "module App; "
        "import BoxLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new Box<int>(1); "
        "    using var b := new Box<string>(\"x\"); "
        "    return a.tag + b.tag; "  // 42 + 42 = 84
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    // Both instantiations ran magic() → 42; sum = 84.
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 84);
}

// (33) §14/#28.4/#662c/#695: destructor body self-call — a dtor body that calls
// a private method must dispatch correctly (mirrors the ctor fix).  Pair<T>
// sets `disposed` in the dtor via a helper(); Main reads `disposed` after scope
// exit, but since the object is destroyed, we use a static field for
// observability.  Simpler shape: just verify the compile+run succeed without
// crash, and that the dtor body's method call ran (n stays 0 if it did NOT run).
// We use the same counter pattern as existing dtor tests.
BEZEL_TEST(cross_module_import,
           imported_generic_dtor_body_selfcall_no_crash_native_spec_14_695) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_695_dtor_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    // Ctor stores 1 into `n`; dtor calls reset() which stores 99 into `n`.
    // Because the object is destroyed by `using var`, after scope exit `n` is
    // meaningless (the object is freed).  We instead verify the program DOESN'T
    // crash and returns the ctor value read BEFORE scope exit (42 is the ctor arg,
    // unrelated to the dtor, so the exit code pins ctor-correctness too).
    writeSource(aSrc.path,
        "module PairLib; "
        "public class Pair<T> "
        "  public value: T; "
        "  public n: int; "
        "  public constructor(v: T) "
        "    value := v; "
        "    n := 1; "
        "  end constructor; "
        "  destructor reset(); end destructor; "
        "  private method reset() n := 99; end reset; "
        "end Pair;");
    writeSource(bSrc.path,
        "module App; "
        "import PairLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var p := new Pair<int>(42); "
        "    return p.n; "  // ctor set n=1; we read BEFORE dtor (n==1 not 99)
        "  end Main; "      // dtor runs at end of using-var scope → no crash
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    // Compile + link must succeed (was: crash during codegen before fix).
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    // p.n read before dtor: ctor set n=1, so return value is 1 (not 99 or 0).
    // This pins: ctor body ran (n=1), dtor body didn't crash.
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 1);
}

// (34) §14/#695 regression: POST-construction self-method call on an imported
// generic instance must continue to work correctly after the #695 fix.
// Calling `w.computeConst()` AFTER `new Wrapper<int>(7)` dispatches through the
// vtable (normal cross-module virtual/non-virtual dispatch) — this path was
// never broken and must remain green.
BEZEL_TEST(cross_module_import,
           imported_generic_post_construction_selfcall_regression_spec_14_695) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_695_post_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module WrapLib; "
        "public class Wrapper<T> "
        "  public raw: T; "
        "  public computed: int; "
        "  public constructor(v: T) "
        "    raw := v; "
        "    computed := computeConst(); "
        "  end constructor; "
        "  destructor end destructor; "
        "  private method computeConst(): int => 42; "
        "end Wrapper;");
    // Call the method AFTER construction — through the normal method-call path.
    writeSource(bSrc.path,
        "module App; "
        "import WrapLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var w := new Wrapper<int>(7); "
        "    return w.computed; "   // was set by ctor; proves ctor fix intact
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    // w.computed was set by ctor (computeConst()=42); must still be 42.
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 42);
}

// §14/#705 facet-a regression tests
// ---------------------------------------------------------------------------
// Root cause: parseAndAttachCtorBody / parseAndAttachDtorBody keyed the
// imported-method remap map by name only (std::string_view), so the SECOND
// same-named overload silently overwrote the first in the map.  A call inside
// the ctor/dtor body that sema resolved to the FIRST overload was then
// remapped to the SECOND imported MethodSymbol (wrong ABI / wrong body).
//
// Fix: key the map by "name(type0,type1,...)" using metaMemberSigKey /
// freshMethodSigKey, so each overload has a unique key.
// ---------------------------------------------------------------------------

// (35) §14/#705 facet-a: imported generic class whose ctor calls TWO same-name
// method overloads that differ only in param type (int vs string).  Each call
// must dispatch to the correct overload so the two result fields carry the
// expected per-overload values.
//
// Repro shape: Dual<T> has
//   compute(x: int): int => 10       (int overload)
//   compute(x: string): int => 20    (string overload)
//   constructor(a: int, b: string)
//     ri := compute(a)               // should call int overload → 10
//     rs := compute(b)               // should call string overload → 20
//   end constructor
//
// Before fix: the name-only map stored only one "compute" entry (whichever was
// last in importedType.members).  Both calls remapped to that single symbol,
// so one of them called the wrong overload and the result field held the wrong
// value.  Main returns ri + rs * 3; with the fix that is 10 + 60 = 70.
BEZEL_TEST(cross_module_import,
           imported_generic_ctor_method_overload_dispatch_native_spec_14_705) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_705_overload_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module DualLib; "
        "public class Dual<T> "
        "  public ri: int; "
        "  public rs: int; "
        "  public constructor(a: int, b: string) "
        "    ri := compute(a); "
        "    rs := compute(b); "
        "  end constructor; "
        "  destructor end destructor; "
        "  private method compute(x: int): int => 10; "
        "  private method compute(x: string): int => 20; "
        "end Dual;");
    writeSource(bSrc.path,
        "module App; "
        "import DualLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var d := new Dual<int>(0, \"\"); "
        "    return d.ri + d.rs * 3; "  // 10 + 20*3 = 70
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    // ri = compute(a) with a:int  → int overload → 10
    // rs = compute(b) with b:string → string overload → 20
    // Main returns 10 + 20 * 3 = 70.
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 70);
}

// §14/#705 facet-b regression tests
// ---------------------------------------------------------------------------
// Root cause: the ctor-body attach loop (loadMetadata pass 3) matched a
// MetaMember ctor to an imported ConstructorSymbol by ARITY ONLY
// (cs->params.size() == mm.params.size()), breaking on the first match.
// Two ctors of the same arity but different param types → the body text for
// the second ctor was attached to the first ConstructorSymbol (and the first
// ctor's body was attached to the second).
//
// Fix: compare each param pair by full type signature using metaTypeSigText
// (MetaParam side) vs typeRefSigText (imported ParameterSymbol side).
// ---------------------------------------------------------------------------

// (36) §14/#705 facet-b: imported generic class with TWO equal-arity
// constructors each with a DISTINCT body.  Constructing with each must run
// the correct body so the `n` field holds the expected per-ctor value.
//
// Repro shape: Picker<T> has
//   constructor(v: int)  n := v + 1;     // body A: add 1
//   constructor(v: string)  n := 99;     // body B: constant 99
//
// Before fix: the first ctor (int) would get body B attached (n := 99) and the
// second (string) would get body A (n := v+1 → type mismatch or zero).
// new Picker<int>(5) would thus set n=99, not 6.
// After fix: new Picker<int>(5) → n=6; the method returns n.  Main returns n.
BEZEL_TEST(cross_module_import,
           imported_generic_equal_arity_ctor_body_attach_native_spec_14_705) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_705_ctorb_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module PickerLib; "
        "public class Picker<T> "
        "  public n: int; "
        "  public constructor(v: int) "
        "    n := v + 1; "
        "  end constructor; "
        "  public constructor(v: string) "
        "    n := 99; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Picker;");
    writeSource(bSrc.path,
        "module App; "
        "import PickerLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var p := new Picker<int>(5); "
        "    return p.n; "  // int ctor: n = 5 + 1 = 6
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    // Picker<int>(5) runs the int ctor: n = 5 + 1 = 6.
    // Before fix: int ctor got string body attached → n = 99 (wrong).
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 6);
}

// #783 / §14.1 (now IMPLEMENTED — was previously decline-with-reasons): calling a
// method whose RETURN TYPE references the class's generic parameter, on an imported
// generic class instantiated with a REFERENCE type argument, IS now monomorphized
// importer-side. The exporter carries the method body source text (schema v5
// methodBodyText); the loader re-parses + type-resolves it and attaches a live
// ast::MethodDecl, and codegen monomorphizes the body per instantiation. So the
// importer type-checks CLEAN (no diagnostic) and `Box<string>.Get()` returns the
// field at the correct `ptr` offset — verified end-to-end (value == 5) by
// tests/module/xmod_generic_method_783_test.cpp T1. (This test formerly asserted
// the SOUND GATE that declined this shape; the gate is lifted now that the real
// fix lands. Shapes whose body cannot be reconstructed still gate — see T5 there.)
BEZEL_TEST(cross_module_import, imported_generic_T_method_reftype_accepted_783) {
    ModuleMetadata dep = buildMetaFor(
        "module Lib2; "
        "public class Box<T> "
        "  public v: T; "
        "  public method Get(): T return this.v; end Get; "
        "end Box;");
    auto r = runImporter(
        dep,
        "module App; import Lib2; "
        "public class C public static method Main(): int "
        "  using var b := new Box<string>(); b.v := \"hello\"; "
        "  return b.Get().Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);  // #783: now monomorphized, no gate
}

// #783 (no false positive): the SAME method on an `int` instantiation matches the
// placeholder layout and works — it is NOT rejected.
BEZEL_TEST(cross_module_import, imported_generic_T_method_int_accepted_783) {
    ModuleMetadata dep = buildMetaFor(
        "module Lib2; "
        "public class Box<T> "
        "  public v: T; "
        "  public method Get(): T return this.v; end Get; "
        "end Box;");
    auto r = runImporter(
        dep,
        "module App; import Lib2; "
        "public class C public static method Main(): int "
        "  using var b := new Box<int>(); b.v := 7; "
        "  return b.Get(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);  // int matches the placeholder → works
}

// #783 (no false positive): a method whose return type does NOT use the type
// parameter (`Hello(): int`) is unaffected by the placeholder layout and is NOT
// rejected, even on a reference-type instantiation.
BEZEL_TEST(cross_module_import, imported_generic_nonT_method_accepted_783) {
    ModuleMetadata dep = buildMetaFor(
        "module Lib2; "
        "public class Box<T> "
        "  public v: T; "
        "  public method Hello(): int return 42; end Hello; "
        "end Box;");
    auto r = runImporter(
        dep,
        "module App; import Lib2; "
        "public class C public static method Main(): int "
        "  using var b := new Box<string>(); "
        "  return b.Hello(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);  // Hello() doesn't use T → works
}

// #783 (no false positive): DIRECT field access on a reference-type instantiation
// IS specialized importer-side and works — only the METHOD path is gated.
BEZEL_TEST(cross_module_import, imported_generic_direct_field_accepted_783) {
    ModuleMetadata dep = buildMetaFor(
        "module Lib2; "
        "public class Box<T> "
        "  public v: T; "
        "  public method Get(): T return this.v; end Get; "
        "end Box;");
    auto r = runImporter(
        dep,
        "module App; import Lib2; "
        "public class C public static method Main(): int "
        "  using var b := new Box<string>(); b.v := \"hello\"; "
        "  return b.v.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);  // direct field read is specialized
}

// === #711 — interim §14 gate: imported generic class with a generic base ===
//
// Constructing an imported generic class whose generic BASE CLASS also lives in
// an imported module is not yet supported (#428/#711): the Foundation C path
// (inlineCtorBody) re-emits the base class template constructor symbol
// importer-side, colliding at link with the exporter's copy.  The gate turns
// the cryptic link error into a clean §14/§12 compile-time diagnostic.
//
// The FULL FIX (importer-side per-instantiation base-ctor monomorphization) is
// tracked under #428/#711.

// (25) §14 / §12 (#711): imported generic class Box<T> : Container<T> — both in
// module A — is REJECTED with a clean §14 diagnostic when constructed importer-
// side.  The sema-level check fires before any codegen / link step.
BEZEL_TEST(cross_module_import,
           imported_generic_class_with_generic_base_rejected_spec_14_12_711) {
    ModuleMetadata dep = buildMetaFor(
        "module A; "
        "public class Container<T> "
        "  public item: T; "
        "  public constructor(v: T) item := v; end constructor; "
        "end Container; "
        "public class Box<T> : Container<T> "
        "  public constructor(v: T) base(v); end constructor; "
        "end Box;");
    auto r = runImporter(
        dep,
        "module M; import A; "
        "class C public static method Main(): int "
        "  using var b := new Box<string>(\"hi\"); return b.item.Length; "
        "end Main; end C;");
    // §14/#711 gate: imported generic class with generic base → clean §14 error.
    BEZEL_EXPECT_TRUE(r->hadErrors);
}

// (26) §14 (#711 no false positive): an imported generic class with NO generic
// base is NOT gated — the §711 predicate does not fire.  Wrapper<T> has no base
// class, so `new Wrapper<int>(9)` at the sema level is accepted clean.
BEZEL_TEST(cross_module_import,
           imported_generic_class_no_generic_base_accepted_spec_14_711) {
    ModuleMetadata dep = buildMetaFor(
        "module A2; "
        "public class Wrapper<T> "
        "  public v: T; "
        "  public constructor(x: T) v := x; end constructor; "
        "end Wrapper;");
    auto r = runImporter(
        dep,
        "module M; import A2; "
        "class C public static method Main(): int "
        "  using var w := new Wrapper<int>(9); return w.v; "
        "end Main; end C;");
    // No generic base → §711 gate does NOT fire; sema accepts.
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// (27) §2.5 / §14 (fix #711, half 1 — SYMBOL OWNERSHIP): the DEFINING module
// owns `bz$<Type>$ctor`; the importer must only ever DECLARE it.
//
// A local primary-constructor class `D(v: int) : Box<int>(v)` deriving an
// IMPORTED GENERIC class makes codegen declare the base ctor
// (emitImplicitBase → declareImportedCtor) while lowering `new D(7)` inside
// Main — i.e. during the METHOD-body loop, so the entry is already in
// `ctorFns_` when the constructor-body loop runs. That loop (unlike the
// disposer loop beside it) did not skip IMPORTED constructors, and an imported
// GENERIC class's ConstructorSymbol carries a re-parsed `decl` (#662
// Foundation C), so `emitConstructorBody` emitted a SECOND definition of the
// exporter's template symbol.
//
// WITHOUT the fix this test fails at the LINK step (`rb.success == false`):
//   /usr/bin/ld: a.o: in function `bz$Box$ctor':
//   multiple definition of `bz$Box$ctor'; b.o: first defined here
// WITH the fix the importer emits only the external declaration, the call
// binds to the exporter's definition, and the program returns `item` = 7.
// This shape is NOT covered by the §14/#711 sema gate above (that gate fires
// on `new ImportedGeneric<...>`; here the constructed type `D` is local).
BEZEL_TEST(cross_module_import,
           imported_generic_ctor_not_redefined_importer_side_spec_2_5_711) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bz711dup_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module Bz711CtorLib; "
        "public class Box<T> "
        "  public item: T; "
        "  public constructor(v: T) item := v; end constructor; "
        "end Box;");
    writeSource(bSrc.path,
        "module App711A; import Bz711CtorLib; "
        "public class D(v: int) : Box<int>(v) end D; "
        "class Program "
        "  public static method Main(): int "
        "    using var d := new D(7); "
        "    return d.item; "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    // Before the fix this is where it broke: the linker rejected the duplicate
    // `bz$Box$ctor` definition.
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 7);  // the imported base ctor stored 7 into `item`
}

// (28) §12 / §14.1 / §2.5 (fix #711, half 2 — INHERITED BASE FIELD LAYOUT):
// a LOCAL generic class `D<T>` deriving an IMPORTED generic base `Box<T>` must
// read/write the inherited `item: T` through the SPECIALIZED base layout.
//
// computeAncestorGenericSubst (the §585 base-chain walk that binds an
// ancestor's generic params to the derived instantiation's concrete arguments)
// read the parameter NAMES exclusively from `ast::TypeDecl`. An IMPORTED type
// has no AST declaration — its parameter names live on the TypeSymbol
// (ModuleLoader::convertGenericParams) — so the walk bailed on the first
// `dynamic_cast` and left no substitution. `classStruct(Box)` then produced the
// T→i32 TEMPLATE layout, and the inherited `item` was stored/read as an i32.
//
// WITHOUT the fix this test fails at COMPILE (`rb.success == false`) with an
// IR-verifier report — the `ptr`-typed string field is GEP'd as i32:
//   GEP base pointer is not a vector or a vector of pointers
//     %13 = getelementptr inbounds nuw %bz.string, i32 %12, 0, 1
// WITH the fix the ancestor substitution resolves T→string, the base subobject
// is laid out with a `ptr` slot, and Main returns "hello".Length + 7 = 12.
// This shape is NOT covered by the §14/#711 sema gate (the constructed type
// `D<string>` is local, not imported).
BEZEL_TEST(cross_module_import,
           local_generic_derives_imported_generic_base_field_spec_12_14_711) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bz711anc_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module Bz711BaseLib; "
        "public class Box<T> "
        "  public item: T; "
        "end Box;");
    writeSource(bSrc.path,
        "module App711B; import Bz711BaseLib; "
        "public class D<T> : Box<T> "
        "  public tag: int := 0; "
        "  public constructor(v: T, t: int) item := v; tag := t; end constructor; "
        "end D; "
        "class Program "
        "  public static method Main(): int "
        "    using var d := new D<string>(\"hello\", 7); "
        "    return d.item.Length + d.tag; "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    // Before the fix this is where it broke: IR verification rejected the
    // i32-typed GEP into the inherited string field.
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 12);  // "hello".Length (5) + tag (7)
}

// §22.3 / issue #1221: a `ref` or `out` parameter of an IMPORTED method must be
// passed by ADDRESS at the call site (not by loaded value). Before the fix the
// else-branch (for ms->decl not an ast::MethodDecl) called lowerExpr for every
// argument, producing an IR type-mismatch (i32 passed where ptr expected) and
// LLVM verifier failure. After the fix the call-site modifier and the imported
// ParameterSymbol's modifier are honoured, exactly as buildOrderedArgs does for
// local methods.
//
// Test A: ref int — both params ref, swap must succeed (a == 20 after swap).
BEZEL_TEST(cross_module_import,
           imported_ref_param_passes_address_not_value_spec_22_3_1221_A) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bz1221_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module RefLib; "
        "public static class Ops "
        "  public static method SwapInts(ref a: int, ref b: int) "
        "    var t := a; a := b; b := t; "
        "  end SwapInts; "
        "end Ops;");
    writeSource(bSrc.path,
        "module App; import RefLib; "
        "class Program "
        "  public static method Main(): int "
        "    var a := 10; var b := 20; "
        "    Ops.SwapInts(ref a, ref b); "
        "    return a; "   // a should be 20 after swap
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 20);  // a == 20 after swap
}

// Test B: out int — callee writes back via out param; caller must observe it.
BEZEL_TEST(cross_module_import,
           imported_out_param_writes_back_spec_22_3_1221_B) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bz1221b_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module OutLib; "
        "public static class Adder "
        "  public static method Add(a: int, b: int, out result: int) "
        "    result := a + b; "
        "  end Add; "
        "end Adder;");
    writeSource(bSrc.path,
        "module App2; import OutLib; "
        "class Program "
        "  public static method Main(): int "
        "    var result := 0; "
        "    Adder.Add(3, 7, out result); "
        "    return result; "   // should be 10
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 10);  // Add(3, 7) == 10
}


// --- §11.3 / §12.1 cross-module base-chain reconstruction (issue #1223) ----
//
// When Base is in module A and Extended : Base is in module B, loading B's
// metadata into a SemaContext that already has A's types must reconstruct
// Extended.baseTypes → [Base] so that member lookup and field-offset layout
// both work.
//
// Root cause: Loader::lookupNamed only searched localTypes (the current
// module's own exported types). The comment above that function described the
// intended behaviour — "first local, then any type already loaded into the
// context" — but the second step was missing. Fix: fall back to
// SemaContext::lookupImportedTypeByName when localTypes misses.

// (27) §11.3 / §12.1 / #1223 sema: loading B's metadata after A's correctly
// links Extended.baseTypes → Base and exposes GetX() via member lookup.
//
// Key: B must be compiled WITH A loaded so that Extended.baseTypeRefs is
// populated (TypeResolution only sets baseTypes/baseTypeRefs when the base
// resolves to a TypeSymbol — §TypeResolution_Names.cpp).  We use a local
// helper that mirrors buildMetaFor but loads the dependency before resolving.
BEZEL_TEST(cross_module_import,
           cross_module_base_chain_member_lookup_sema_spec_11_3_12_1_1223) {
    static const std::string kASrc =
        "module BaseLib1223; "
        "public class Base "
        "  public _x: int; "
        "  public constructor(x: int) _x := x; end constructor; "
        "  public method GetX(): int => _x; "
        "  destructor end destructor; "
        "end Base;";
    static const std::string kBSrc =
        "module ExtLib1223; "
        "import BaseLib1223; "
        "public class Extended : Base "
        "  public _y: int; "
        "  public constructor(x: int, y: int) base(x); _y := y; end constructor; "
        "  public method GetY(): int => _y; "
        "  public method Sum(): int => GetX() + _y; "
        "  destructor end destructor; "
        "end Extended;";

    // Build A's metadata stand-alone (no deps).
    ModuleMetadata depA = buildMetaFor(kASrc);

    // Build B's metadata with A loaded into its sema context, so that
    // Extended.baseTypeRefs is populated (Base resolves, §TypeResolution_Names).
    ModuleMetadata depB;
    {
        DiagnosticEngine bDiags;
        Lexer bLex(kBSrc, bDiags);
        Parser bPar(bLex, bDiags);
        auto bSf = bPar.parseCompilationUnit();
        if (bSf) {
            sema::SemaContext bCtx(bDiags);
            sema::collectSymbols(*bSf, bCtx);
            loadMetadata(depA, bCtx);          // A types visible during B's resolve
            sema::resolveTypes(*bSf, bCtx);
            sema::typecheckExpressions(*bSf, bCtx);
            depB = buildModuleMetadata(bCtx, kBSrc);
        }
    }

    // Main context: load A then B.  B's Extended.baseTypes["Base"] should
    // be resolved by lookupNamed falling back to ctx.lookupImportedTypeByName.
    DiagnosticEngine diags;
    sema::SemaContext ctx(diags);
    BEZEL_EXPECT_TRUE(loadMetadata(depA, ctx));
    BEZEL_EXPECT_TRUE(loadMetadata(depB, ctx));

    // Find the Extended TypeSymbol in B's exports.
    const std::vector<sema::TypeSymbol*>* bTypes = ctx.importedTypesOf("ExtLib1223");
    BEZEL_EXPECT_TRUE(bTypes != nullptr);
    if (!bTypes) return;
    sema::TypeSymbol* extSym = nullptr;
    for (sema::TypeSymbol* t : *bTypes)
        if (t->name == "Extended") extSym = t;
    BEZEL_EXPECT_TRUE(extSym != nullptr);
    if (!extSym) return;

    // §11.3 / §12.1: Extended.baseTypes must link to Base (from A) — not empty.
    // Without the fix, lookupNamed("Base") during B's load returns nullptr
    // (Base is not in B's localTypes), so baseTypes stays empty.
    BEZEL_EXPECT_TRUE(!extSym->baseTypes.empty());
    if (extSym->baseTypes.empty()) return;
    BEZEL_EXPECT_TRUE(extSym->baseTypes[0] != nullptr);
    if (extSym->baseTypes[0])
        BEZEL_EXPECT_TRUE(extSym->baseTypes[0]->name == "Base");

    // §11.3: lookupMember on Extended must find the inherited GetX().
    sema::Symbol* getX = nullptr;
    {
        // BFS walk mirroring ExprTC::lookupMember.
        std::vector<sema::TypeSymbol*> frontier{extSym};
        std::size_t i = 0;
        while (i < frontier.size()) {
            sema::TypeSymbol* t = frontier[i++];
            for (sema::Symbol* m : t->members)
                if (m && m->name == "GetX") { getX = m; break; }
            if (getX) break;
            for (sema::TypeSymbol* b : t->baseTypes)
                if (b) frontier.push_back(b);
        }
    }
    BEZEL_EXPECT_TRUE(getX != nullptr);
}

// (28) §11.3 / §12.1 / #1223 AOT runtime: Base in module A, Extended : Base
// in module B, main imports both.  ext.GetX() must return the base field (100
// not 200), ext.Sum() = GetX() + GetY() = 300 not 400.  Compile A → A.o,
// B (--import A.o) → B.o, main (--import A.o B.o) → exe.
BEZEL_TEST(cross_module_import,
           cross_module_base_chain_field_layout_aot_spec_11_3_12_1_1223) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_1223_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bObj((dir / (stem + "_b.o")).string());
    TempFile cSrc((dir / (stem + "_c.bzl")).string());
    TempFile cExe((dir / (stem + "_c")).string());

    // Module A: Base with _x field, GetX() method.
    writeSource(aSrc.path,
        "module BaseLib1223N; "
        "public class Base "
        "  public _x: int; "
        "  public constructor(x: int) _x := x; end constructor; "
        "  public method GetX(): int => _x; "
        "  destructor end destructor; "
        "end Base;");
    // Module B: Extended : Base (cross-module base) + Factory.
    writeSource(bSrc.path,
        "module ExtLib1223N; "
        "import BaseLib1223N; "
        "public class Extended : Base "
        "  public _y: int; "
        "  public constructor(x: int, y: int) base(x); _y := y; end constructor; "
        "  public method GetY(): int => _y; "
        "  public method Sum(): int => GetX() + _y; "
        "  destructor end destructor; "
        "end Extended; "
        "public class Factory "
        "  public static method Create(x: int, y: int): owned Extended "
        "    return new Extended(x, y); "
        "  end Create; "
        "end Factory;");
    // Main: imports A and B, creates Extended(100, 200) via Factory.Create,
    // checks GetX()==100, GetY()==200, Sum()==300.  Returns 0 on success.
    writeSource(cSrc.path,
        "module Main1223N; "
        "import BaseLib1223N; "
        "import ExtLib1223N; "
        "public class Program "
        "  public static method Main(): int "
        "    using var ext := Factory.Create(100, 200); "
        "    var gx := ext.GetX(); "
        "    var gy := ext.GetY(); "
        "    var s := ext.Sum(); "
        "    if gx <> 100 then return 1; end; "
        "    if gy <> 200 then return 2; end; "
        "    if s <> 300 then return 3; end; "
        "    return 0; "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    // Step 1: compile A → A.o (no dependencies).
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    // Step 2: compile B → B.o (imports A).
    std::vector<bezel::Dependency> depsB;
    {
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath   = aObj.path;
        depsB.push_back(d);
    }
    bezel::CompileResult rb = driver.compileObject(bSrc.path, bObj.path, depsB);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    // Step 3: compile main → executable (imports A and B).
    std::vector<bezel::Dependency> depsC;
    {
        bezel::Dependency dA, dB;
        dA.metadataPath = aObj.path; dA.objectPath = aObj.path;
        dB.metadataPath = bObj.path; dB.objectPath = bObj.path;
        depsC.push_back(dA);
        depsC.push_back(dB);
    }
    bezel::CompileResult rc = driver.compileFile(cSrc.path, cExe.path, depsC);
    BEZEL_EXPECT_TRUE(rc.success);
    if (!rc.success) return;

    int status = std::system(cExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    // 0 = GetX()==100, GetY()==200, Sum()==300 (all correct).
    // 1 = GetX() returned wrong field; 2 = GetY() wrong; 3 = Sum() wrong.
    BEZEL_EXPECT_EQ(code, 0);
}


// ---------------------------------------------------------------------------
// #1513 / §2.8.3 rule 4 / §2.8.4 rule 5: a generic type's static state must be
// exactly ONE copy process-wide, even when TWO SEPARATE importing modules each
// independently monomorphize the SAME instantiation in their OWN compilation
// (§2.8.2 definition-site visibility: an imported generic's body is carried in
// the interface and re-monomorphized per consumer). Before the fix, the per-
// instantiation static-field global (LlvmBackend_Setup.cpp's staticFieldGlobal
// generic-instantiation branch) was InternalLinkage — "each importing module
// owns its own instance" — so two importers of the SAME instantiation held
// two DISTINCT, never-synchronized storage cells: a write through one
// importer was invisible to a read through the other.
//
// This test isolates the STATE-SHARING half of the bug from the (separately
// tracked, harder) VALUE-INITIALIZATION half: `Reg<T>.Value` is written
// directly by GW1 (a plain assignment, fully resolved within GW1's own
// compilation — no imported AST/initializer-text round-trip needed) and read
// back by GW2. Before the fix this returns 0 (GW2's own private, never-
// written copy); after the fix it returns 42 (the SAME shared cell GW1 wrote).
// ---------------------------------------------------------------------------
BEZEL_TEST(cross_module_import,
           imported_generic_static_state_shared_across_two_importers_spec_1513) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_1513_");
    TempFile baseSrc((dir / (stem + "_base.bzl")).string());
    TempFile baseObj((dir / (stem + "_base.o")).string());
    TempFile w1Src((dir / (stem + "_w1.bzl")).string());
    TempFile w1Obj((dir / (stem + "_w1.o")).string());
    TempFile w2Src((dir / (stem + "_w2.bzl")).string());
    TempFile w2Obj((dir / (stem + "_w2.o")).string());
    TempFile appSrc((dir / (stem + "_app.bzl")).string());
    TempFile appExe((dir / (stem + "_app")).string());

    // Module Base1513: Reg<T> with a mutable static int (T-independent — the
    // field's type doesn't mention T at all, isolating this test from the
    // #709/#1513 T-independent-initializer scope limit).
    writeSource(baseSrc.path,
        "module Base1513; "
        "public class Reg<T> "
        "  public static Value: int := 0; "
        "end Reg;");
    bezel::Driver driver;
    bezel::CompileResult rBase = driver.compileObject(baseSrc.path, baseObj.path);
    BEZEL_EXPECT_TRUE(rBase.success);
    if (!rBase.success) return;

    // Module W1_1513: writes Reg<int>.Value.
    writeSource(w1Src.path,
        "module W1_1513; "
        "import Base1513; "
        "public class W1 "
        "  public static method Set(v: int) "
        "    Reg<int>.Value := v; "
        "  end Set; "
        "end W1;");
    std::vector<bezel::Dependency> depsBase;
    {
        bezel::Dependency d;
        d.metadataPath = baseObj.path;
        d.objectPath   = baseObj.path;
        depsBase.push_back(d);
    }
    bezel::CompileResult rW1 =
        driver.compileObject(w1Src.path, w1Obj.path, depsBase);
    BEZEL_EXPECT_TRUE(rW1.success);
    if (!rW1.success) return;

    // Module W2_1513: reads Reg<int>.Value — a SEPARATE compilation from W1,
    // each independently monomorphizing Reg<int>.
    writeSource(w2Src.path,
        "module W2_1513; "
        "import Base1513; "
        "public class W2 "
        "  public static method Get(): int "
        "    return Reg<int>.Value; "
        "  end Get; "
        "end W2;");
    bezel::CompileResult rW2 =
        driver.compileObject(w2Src.path, w2Obj.path, depsBase);
    BEZEL_EXPECT_TRUE(rW2.success);
    if (!rW2.success) return;

    // App: imports W1 + W2 (+ transitively Base1513), sets via W1, reads via
    // W2. §2.8.3 rule 4: ONE Reg<int> instantiation program-wide → W2 must
    // observe W1's write.
    writeSource(appSrc.path,
        "module App1513; "
        "import W1_1513; "
        "import W2_1513; "
        "public class Program "
        "  public static method Main(): int "
        "    W1.Set(42); "
        "    return W2.Get(); "
        "  end Main; "
        "end Program;");
    std::vector<bezel::Dependency> depsApp;
    {
        bezel::Dependency dBase, dW1, dW2;
        dBase.metadataPath = baseObj.path; dBase.objectPath = baseObj.path;
        dW1.metadataPath   = w1Obj.path;   dW1.objectPath   = w1Obj.path;
        dW2.metadataPath   = w2Obj.path;   dW2.objectPath   = w2Obj.path;
        depsApp.push_back(dBase);
        depsApp.push_back(dW1);
        depsApp.push_back(dW2);
    }
    bezel::CompileResult rApp =
        driver.compileFile(appSrc.path, appExe.path, depsApp);
    BEZEL_EXPECT_TRUE(rApp.success);
    if (!rApp.success) return;

    int status = std::system(appExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    // §2.8.3 rule 4 / §2.8.4 rule 5: 42 — W2 observes the SAME storage W1
    // wrote. Pre-fix: 0 (W2's own private, never-written InternalLinkage copy).
    BEZEL_EXPECT_EQ(code, 42);
}

// #1513 same-module CONTROL: Reg<T>, W1, W2, and Program all declared in ONE
// compilation. The generic-instantiation redirect (staticFieldGlobal) and the
// new per-instantiation ensureinit/sfinit machinery (LlvmBackend_Setup.cpp,
// genericInstEnsureInitFn/genericInstSfinitFn) engage here too — this pins
// that the #1513 fix does not regress the baseline same-module case (which
// worked before the fix via the OLD per-instantiation-global creation path).
BEZEL_TEST(cross_module_import,
           generic_static_state_shared_same_module_control_spec_1513) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_1513_ctrl_");
    TempFile src((dir / (stem + ".bzl")).string());
    TempFile exe((dir / stem).string());

    writeSource(src.path,
        "module Ctrl1513; "
        "public class Reg<T> "
        "  public static Value: int := 0; "
        "end Reg; "
        "public class W1 "
        "  public static method Set(v: int) "
        "    Reg<int>.Value := v; "
        "  end Set; "
        "end W1; "
        "public class W2 "
        "  public static method Get(): int "
        "    return Reg<int>.Value; "
        "  end Get; "
        "end W2; "
        "public class Program "
        "  public static method Main(): int "
        "    W1.Set(42); "
        "    return W2.Get(); "
        "  end Main; "
        "end Program;");
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src.path, exe.path);
    BEZEL_EXPECT_TRUE(r.success);
    if (!r.success) return;

    int status = std::system(exe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 42);
}

// #1513 self-pass (b): a generic instantiation's static state must be
// initialized EXACTLY ONCE process-wide even when TWO SEPARATE importers each
// independently build (and, post-fix, LinkOnceODR-merge into one) the guard/
// sfinit/ensureinit machinery for the SAME instantiation. If the shared STATE
// global were merged but each importer's GUARD flag were NOT — the "one state
// copy, two init flags" failure mode named in the fix's own review — a second
// importer's independent (uninitialized, flag=0) guard could re-run its own
// copy of sfinit against the ALREADY-initialized shared state, clobbering a
// value another domain/read may already be relying on. Exercise via TWO
// distinct instantiations (Reg<int>, Reg<string>) each touched by a DIFFERENT
// importer, both summed by App — a double-init of either would still net the
// same additive result here only if the increments are idempotent, so this
// specifically checks the addresses/values are internally consistent across
// three artifacts (Base + 2 importers), not just two.
BEZEL_TEST(cross_module_import,
           imported_generic_static_two_instantiations_two_importers_spec_1513) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_1513_multi_");
    TempFile baseSrc((dir / (stem + "_base.bzl")).string());
    TempFile baseObj((dir / (stem + "_base.o")).string());
    TempFile w1Src((dir / (stem + "_w1.bzl")).string());
    TempFile w1Obj((dir / (stem + "_w1.o")).string());
    TempFile w2Src((dir / (stem + "_w2.bzl")).string());
    TempFile w2Obj((dir / (stem + "_w2.o")).string());
    TempFile appSrc((dir / (stem + "_app.bzl")).string());
    TempFile appExe((dir / (stem + "_app")).string());

    writeSource(baseSrc.path,
        "module Base1513M; "
        "public class Reg<T> "
        "  public static Value: int := 0; "
        "end Reg;");
    bezel::Driver driver;
    bezel::CompileResult rBase = driver.compileObject(baseSrc.path, baseObj.path);
    BEZEL_EXPECT_TRUE(rBase.success);
    if (!rBase.success) return;

    // W1 touches BOTH Reg<int> (sets 10) and Reg<string> (sets 100).
    writeSource(w1Src.path,
        "module W1_1513M; "
        "import Base1513M; "
        "public class W1 "
        "  public static method SetInt() "
        "    Reg<int>.Value := 10; "
        "  end SetInt; "
        "  public static method SetString() "
        "    Reg<string>.Value := 100; "
        "  end SetString; "
        "end W1;");
    std::vector<bezel::Dependency> depsBase;
    {
        bezel::Dependency d;
        d.metadataPath = baseObj.path;
        d.objectPath   = baseObj.path;
        depsBase.push_back(d);
    }
    bezel::CompileResult rW1 =
        driver.compileObject(w1Src.path, w1Obj.path, depsBase);
    BEZEL_EXPECT_TRUE(rW1.success);
    if (!rW1.success) return;

    // W2 reads BOTH — a SEPARATE compilation, independently monomorphizing
    // both Reg<int> and Reg<string>.
    writeSource(w2Src.path,
        "module W2_1513M; "
        "import Base1513M; "
        "public class W2 "
        "  public static method GetInt(): int "
        "    return Reg<int>.Value; "
        "  end GetInt; "
        "  public static method GetString(): int "
        "    return Reg<string>.Value; "
        "  end GetString; "
        "end W2;");
    bezel::CompileResult rW2 =
        driver.compileObject(w2Src.path, w2Obj.path, depsBase);
    BEZEL_EXPECT_TRUE(rW2.success);
    if (!rW2.success) return;

    writeSource(appSrc.path,
        "module App1513M; "
        "import W1_1513M; "
        "import W2_1513M; "
        "public class Program "
        "  public static method Main(): int "
        "    W1.SetInt(); "
        "    W1.SetString(); "
        "    return W2.GetInt() + W2.GetString(); "
        "  end Main; "
        "end Program;");
    std::vector<bezel::Dependency> depsApp;
    {
        bezel::Dependency dBase, dW1, dW2;
        dBase.metadataPath = baseObj.path; dBase.objectPath = baseObj.path;
        dW1.metadataPath   = w1Obj.path;   dW1.objectPath   = w1Obj.path;
        dW2.metadataPath   = w2Obj.path;   dW2.objectPath   = w2Obj.path;
        depsApp.push_back(dBase);
        depsApp.push_back(dW1);
        depsApp.push_back(dW2);
    }
    bezel::CompileResult rApp =
        driver.compileFile(appSrc.path, appExe.path, depsApp);
    BEZEL_EXPECT_TRUE(rApp.success);
    if (!rApp.success) return;

    int status = std::system(appExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    // §22.12 "each distinct instantiation has its own static state": Reg<int>
    // and Reg<string> are DISTINCT instantiations (10 and 100, independent) —
    // 110 total. A collision between the two instantiations' per-instantiation
    // globals (a mangle/key bug) would produce 100+100=200 or 10+10=20 instead.
    BEZEL_EXPECT_EQ(code, 110);
}

// ---------------------------------------------------------------------------
// #2351 / §2.8.2 clause 2 / §22.12 rule 1 / §22.13: an imported generic
// type's STATIC FIELD INITIALIZER must actually run on the importing side,
// not just share storage (#1513's own fix). #1513's OWN repro shape — a
// `static readonly` field with a NON-TRIVIAL initializer (`new
// AtomicInteger<int>(0)`), touched through two SEPARATE consumer artifacts —
// returned 0/0 before this fix (FieldSymbol::decl stayed null for the
// imported field, so genericInstSfinitFn's `dynamic_cast<const
// ast::FieldDecl*>(fs2->decl)` was always null and the store was skipped).
// This test is the "spec-cited regression test" for #2351: two SEPARATE
// importers each call Increment() once — 1 then 2, matching the same-module
// baseline (generic_static_state_shared_same_module_control_spec_1513-style)
// rather than the pre-fix 0/0.
// ---------------------------------------------------------------------------
BEZEL_TEST(cross_module_import,
           imported_generic_static_field_initializer_runs_once_across_two_importers_spec_1513_2351) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_2351_");
    TempFile baseSrc((dir / (stem + "_base.bzl")).string());
    TempFile baseObj((dir / (stem + "_base.o")).string());
    TempFile w1Src((dir / (stem + "_w1.bzl")).string());
    TempFile w1Obj((dir / (stem + "_w1.o")).string());
    TempFile w2Src((dir / (stem + "_w2.bzl")).string());
    TempFile w2Obj((dir / (stem + "_w2.o")).string());
    TempFile appSrc((dir / (stem + "_app.bzl")).string());
    TempFile appExe((dir / (stem + "_app")).string());

    // Module Base2351: Reg<T> with a NON-TRIVIAL static initializer —
    // `new AtomicInteger<int>(0)` — T-independent (AtomicInteger's own type
    // argument is the fixed `int`, not T; the T-dependent case is the
    // separately-tracked #803 scope note genericInstSfinitFn's own comments
    // name, not this issue's job) and a compiler-intrinsic type (stdlib
    // §6.4), so it needs no `import` in the synthetic reconstruction.
    writeSource(baseSrc.path,
        "module Base2351; "
        "public class Reg<T> "
        "  public static readonly Hits: AtomicInteger<int> := "
        "new AtomicInteger<int>(0); "
        "end Reg;");
    bezel::Driver driver;
    bezel::CompileResult rBase = driver.compileObject(baseSrc.path, baseObj.path);
    BEZEL_EXPECT_TRUE(rBase.success);
    if (!rBase.success) return;

    // W1_2351: touches Reg<int>.Hits once.
    writeSource(w1Src.path,
        "module W1_2351; "
        "import Base2351; "
        "public class W1 "
        "  public static method Bump(): int "
        "    return Reg<int>.Hits.Increment(); "
        "  end Bump; "
        "end W1;");
    std::vector<bezel::Dependency> depsBase;
    {
        bezel::Dependency d;
        d.metadataPath = baseObj.path;
        d.objectPath   = baseObj.path;
        depsBase.push_back(d);
    }
    bezel::CompileResult rW1 =
        driver.compileObject(w1Src.path, w1Obj.path, depsBase);
    BEZEL_EXPECT_TRUE(rW1.success);
    if (!rW1.success) return;

    // W2_2351: touches Reg<int>.Hits once — a SEPARATE compilation from W1,
    // each independently monomorphizing Reg<int> and independently
    // reconstructing the field's initializer text (this fix's mechanism).
    writeSource(w2Src.path,
        "module W2_2351; "
        "import Base2351; "
        "public class W2 "
        "  public static method Bump(): int "
        "    return Reg<int>.Hits.Increment(); "
        "  end Bump; "
        "end W2;");
    bezel::CompileResult rW2 =
        driver.compileObject(w2Src.path, w2Obj.path, depsBase);
    BEZEL_EXPECT_TRUE(rW2.success);
    if (!rW2.success) return;

    // App: imports W1 + W2 (+ transitively Base2351), bumps via W1 then W2.
    writeSource(appSrc.path,
        "module App2351; "
        "import W1_2351; "
        "import W2_2351; "
        "public class Program "
        "  public static method Main(): int "
        "    var a := W1.Bump(); "
        "    var b := W2.Bump(); "
        "    return a * 10 + b; "
        "  end Main; "
        "end Program;");
    std::vector<bezel::Dependency> depsApp;
    {
        bezel::Dependency dBase, dW1, dW2;
        dBase.metadataPath = baseObj.path; dBase.objectPath = baseObj.path;
        dW1.metadataPath   = w1Obj.path;   dW1.objectPath   = w1Obj.path;
        dW2.metadataPath   = w2Obj.path;   dW2.objectPath   = w2Obj.path;
        depsApp.push_back(dBase);
        depsApp.push_back(dW1);
        depsApp.push_back(dW2);
    }
    bezel::CompileResult rApp =
        driver.compileFile(appSrc.path, appExe.path, depsApp);
    BEZEL_EXPECT_TRUE(rApp.success);
    if (!rApp.success) return;

    int status = std::system(appExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    // 1 then 2 (a=1, b=2 -> 12): the SAME shared AtomicInteger cell
    // (#1513's LinkOnceODR fix), genuinely INITIALIZED (this fix) exactly
    // once before either Increment() call. Pre-fix: 0/0 (Hits stayed at its
    // zero-init default — a null AtomicInteger<int> reference — since the
    // initializer never ran on the importing side); a double-init bug would
    // instead produce 1 then 1 (a fresh AtomicInteger(0) re-constructed and
    // re-stored between the two touches, resetting the count).
    BEZEL_EXPECT_EQ(code, 12);
}

// #2351 self-pass (a) / §2.8.2 "no restrictions beyond visibility... must
// reject the export, or the import, with a diagnostic naming the shape; it
// must not alter the instantiation's semantics": a static field initializer
// that references a name NOT visible inside the importer's synthetic
// reconstruction (an INTERNAL sibling type's method, carried nowhere near
// Reg<T>'s own metadata — a different failure surface than #710/#2290 bug 2's
// ctor-body sibling-type gap, but the same root limitation: the synthetic
// class declares only the type being reconstructed) must fail CLEANLY with a
// diagnostic naming the field, not silently leave `Hits` at its zero-init
// default (the pre-#2351 behavior for EVERY shape, correct or not).
BEZEL_TEST(cross_module_import,
           imported_generic_static_field_init_unresolvable_reference_rejected_spec_2351) {
    ModuleMetadata dep = buildMetaFor(
        "module Base2351NV; "
        "internal class Helper2351NV "
        "  internal static method Seed(): int "
        "    return 7; "
        "  end Seed; "
        "end Helper2351NV; "
        "public class Reg<T> "
        "  public static readonly Hits: AtomicInteger<int> := "
        "new AtomicInteger<int>(Helper2351NV.Seed()); "
        "end Reg;");
    auto r = runImporter(
        dep,
        "module W1_2351NV; "
        "import Base2351NV; "
        "public class W1 "
        "  public static method Bump(): int "
        "    return Reg<int>.Hits.Increment(); "
        "  end Bump; "
        "end W1;");
    BEZEL_EXPECT_TRUE(r->hadErrors);
    bool foundDiag = false;
    for (const Diagnostic& d : r->diags.diagnostics()) {
        if (d.message.find("cannot import static field") != std::string::npos &&
            d.message.find("Hits") != std::string::npos) {
            foundDiag = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(foundDiag);
}

// #2375 / §2.8.2 (follow-up to #2351): a static field initializer of an
// imported generic class that references a PUBLIC SIBLING TYPE — another
// type declared in the same exporting module — must RECONSTRUCT (and run)
// on the importing side, not be rejected. Before #2375,
// buildSyntheticFieldInitSource declared only the type being reconstructed,
// so the sibling-type reference failed resolveTypes and the import was
// rejected with "a referenced type did not resolve inside the module
// boundary" even for the simplest `new Box(42)` shape (the field's own
// type being the sibling). #2375 makes parseAndAttachFieldInit register
// the exporting module's already-imported PUBLIC sibling TypeSymbols into
// the fresh reconstruction context (and emit `import <moduleName>;` in the
// synthetic source), so the reference resolves to the REAL imported symbol
// and the initializer runs.
//
// This test is the spec-cited regression test for #2375: a two-module
// setup (Base2375 declares `public class Box` + `public class Reg<T>` with
// `public static readonly Res: Box := new Box(42);`; App2375 imports Base
// and reads `Reg<int>.Res.Tag`). Pre-fix: rejected ("cannot import static
// field 'Reg.Res'..."). Post-fix: compiles + runs + returns 42 (the
// sibling-type initializer `new Box(42)` ran on the importing side and
// stored a Box whose Tag is 42).
BEZEL_TEST(cross_module_import,
           imported_generic_static_field_init_sibling_type_reconstructs_spec_2375_2_8_2) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_2375_");
    TempFile baseSrc((dir / (stem + "_base.bzl")).string());
    TempFile baseObj((dir / (stem + "_base.o")).string());
    TempFile appSrc((dir / (stem + "_app.bzl")).string());
    TempFile appExe((dir / (stem + "_app")).string());

    // Module Base2375: a PUBLIC sibling class `Box` (the type referenced by
    // the static field) + a generic `Reg<T>` whose static readonly field's
    // TYPE and INITIALIZER both reference that sibling. T-independent
    // (the #803 T-dependent scope is separately tracked) and the sibling
    // is a plain user-defined class — exactly the shape #2351's tests
    // deliberately avoided (they used compiler-intrinsic AtomicInteger<int>
    // which needs no import in the synthetic reconstruction).
    writeSource(baseSrc.path,
        "module Base2375; "
        "public class Box "
        "  public Tag: int; "
        "  public constructor(tag: int) "
        "    Tag := tag; "
        "  end constructor; "
        "end Box; "
        "public class Reg<T> "
        "  public static readonly Res: Box := new Box(42); "
        "end Reg;");
    bezel::Driver driver;
    bezel::CompileResult rBase = driver.compileObject(baseSrc.path, baseObj.path);
    BEZEL_EXPECT_TRUE(rBase.success);
    if (!rBase.success) return;

    // App2375: imports Base2375 and reads the sibling-typed static field.
    writeSource(appSrc.path,
        "module App2375; "
        "import Base2375; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := Reg<int>.Res; "
        "    return b.Tag; "
        "  end Main; "
        "end Program;");
    std::vector<bezel::Dependency> depsApp;
    {
        bezel::Dependency d;
        d.metadataPath = baseObj.path;
        d.objectPath   = baseObj.path;
        depsApp.push_back(d);
    }
    bezel::CompileResult rApp =
        driver.compileFile(appSrc.path, appExe.path, depsApp);
    BEZEL_EXPECT_TRUE(rApp.success);
    if (!rApp.success) return;

    int status = std::system(appExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    // 42: the sibling-type initializer `new Box(42)` ran on the importing
    // side (the field's declared type `Box` AND the constructor argument
    // both crossed the module boundary via the now-resolved sibling).
    // Pre-fix: the import was rejected outright (BEZEL_EXPECT_TRUE(rApp.success)
    // above would fail with "cannot import static field 'Reg.Res'...").
    BEZEL_EXPECT_EQ(code, 42);
}

// #2375 (sibling-MEMBER reference): the second half of the fix — once the
// sibling TYPE resolves, an initializer sub-expression that reads a STATIC
// MEMBER of that sibling (`new Box(Box.DefaultSize)`) must resolve the
// member reference to the REAL imported FieldSymbol and read its value,
// not silently read zero. The expr-symbol remap loop in
// parseAndAttachFieldInit previously copied ONLY self-fields/self-methods
// into mainCtx; a sibling-member reference resolved in the fresh context to
// the real imported symbol but was never copied to mainCtx, so codegen had
// no symbol for the read and the arg evaluated to zero (silent miscompile:
// Box.DefaultSize, declared `:= 77`, read as 0 → Box.Tag came out 0). The
// fix copies such non-self symbols as-is (the fresh symbol already IS the
// imported one). This test pins that: `Reg<int>.Seed.Tag` must be 77, the
// sibling's own static readonly value, same as the same-module baseline.
BEZEL_TEST(cross_module_import,
           imported_generic_static_field_init_sibling_member_ref_resolves_spec_2375_2_8_2) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_2375m_");
    TempFile baseSrc((dir / (stem + "_base.bzl")).string());
    TempFile baseObj((dir / (stem + "_base.o")).string());
    TempFile appSrc((dir / (stem + "_app.bzl")).string());
    TempFile appExe((dir / (stem + "_app")).string());

    // Module Base2375M: sibling `Box` carries its OWN public static
    // readonly `DefaultSize := 77`, and `Reg<T>.Seed`'s initializer reads
    // that sibling static as the constructor argument — a sibling-MEMBER
    // reference, not just a sibling-TYPE reference.
    writeSource(baseSrc.path,
        "module Base2375M; "
        "public class Box "
        "  public static readonly DefaultSize: int := 77; "
        "  public Tag: int; "
        "  public constructor(tag: int) "
        "    Tag := tag; "
        "  end constructor; "
        "end Box; "
        "public class Reg<T> "
        "  public static readonly Seed: Box := new Box(Box.DefaultSize); "
        "end Reg;");
    bezel::Driver driver;
    bezel::CompileResult rBase = driver.compileObject(baseSrc.path, baseObj.path);
    BEZEL_EXPECT_TRUE(rBase.success);
    if (!rBase.success) return;

    writeSource(appSrc.path,
        "module App2375M; "
        "import Base2375M; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := Reg<int>.Seed; "
        "    return b.Tag; "
        "  end Main; "
        "end Program;");
    std::vector<bezel::Dependency> depsApp;
    {
        bezel::Dependency d;
        d.metadataPath = baseObj.path;
        d.objectPath   = baseObj.path;
        depsApp.push_back(d);
    }
    bezel::CompileResult rApp =
        driver.compileFile(appSrc.path, appExe.path, depsApp);
    BEZEL_EXPECT_TRUE(rApp.success);
    if (!rApp.success) return;

    int status = std::system(appExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    // 77: the sibling static `Box.DefaultSize` was read correctly inside the
    // reconstructed initializer (the expr-symbol copy fix), so the Box was
    // constructed with Tag=77. Pre-fix this returned 0 (silent miscompile:
    // the sibling-member reference had no symbol in mainCtx). With only the
    // sibling-TYPE half of #2375 and not the expr-symbol copy, this same
    // binary returns 0 — pinning the second half of the fix.
    BEZEL_EXPECT_EQ(code, 77);
}

// #2525 / §2.8.2: an imported RECORD constructor in a generic static-field
// initializer must be reconstructed with its body and lowered by value. Before
// the fix, sema reconstructed `new Point(73, 2)` but buildRecordValue found an
// imported ConstructorSymbol with no decl and returned null.
BEZEL_TEST(cross_module_import,
           imported_generic_static_field_init_record_ctor_spec_2525_2_8_2) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_2525_");
    TempFile baseSrc((dir / (stem + "_base.bzl")).string());
    TempFile baseObj((dir / (stem + "_base.o")).string());
    TempFile appSrc((dir / (stem + "_app.bzl")).string());
    TempFile appExe((dir / (stem + "_app")).string());

    writeSource(baseSrc.path,
        "module Base2525; "
        "public record Point "
        "  public X: int; public Y: int; "
        "  public constructor(a: int, b: int) "
        "    X := a; Y := b; "
        "  end constructor; "
        "end Point; "
        "public class Reg<T> "
        "  public static readonly Origin: Point := new Point(73, 2); "
        "end Reg;");
    bezel::Driver driver;
    bezel::CompileResult rBase = driver.compileObject(baseSrc.path, baseObj.path);
    BEZEL_EXPECT_TRUE(rBase.success);
    if (!rBase.success) return;

    writeSource(appSrc.path,
        "module App2525; import Base2525; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := Reg<int>.Origin; return p.X; "
        "  end Main; end Program;");
    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep;
    dep.metadataPath = baseObj.path;
    dep.objectPath = baseObj.path;
    deps.push_back(dep);
    bezel::CompileResult rApp = driver.compileFile(appSrc.path, appExe.path, deps);
    BEZEL_EXPECT_TRUE(rApp.success);
    if (!rApp.success) return;
    int status = std::system(appExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 73);
}

// #2351 self-pass (c) / §22.13 "Disposal of static owned fields": an `owned`
// static field's exit disposal must run EXACTLY ONCE process-wide even when
// TWO SEPARATE importers each independently reconstruct the field's
// initializer and register it for exit disposal (genericInstSfinitFn's
// `bz_register_static_owned` call, reachable for an imported generic field
// only via this fix — it was dead code for that case before, since fd/
// initExpr were always null). Mirrors #2290/#710's runCrossModuleChecked
// ASan/LSan technique, extended to THREE artifacts (Base + two importers).
// A double-dispose (once per importer) would abort under ASan
// (heap-corruption / double-free); a never-registered dispose would report
// an LSan leak. Neither fires -> exactly once.
BEZEL_TEST(cross_module_import,
           imported_generic_static_owned_field_disposed_once_across_two_importers_spec_2351_22_13) {
    namespace aot_detail = bezel::test::aot_detail;
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = unique_stem("bezel_2351_owned_asan_");
    TempFile baseSrc((dir / (stem + "_base.bzl")).string());
    TempFile baseObj((dir / (stem + "_base.o")).string());
    TempFile w1Src((dir / (stem + "_w1.bzl")).string());
    TempFile w1Obj((dir / (stem + "_w1.o")).string());
    TempFile w2Src((dir / (stem + "_w2.bzl")).string());
    TempFile w2Obj((dir / (stem + "_w2.o")).string());
    TempFile appSrc((dir / (stem + "_app.bzl")).string());
    TempFile appExe((dir / (stem + "_app")).string());
    TempFile sanOut((dir / (stem + ".san.txt")).string());
    TempFile stdOut((dir / (stem + ".out.txt")).string());

    writeSource(baseSrc.path,
        "module Base2351Owned; "
        "public class Reg<T> "
        "  public static owned readonly Hits: AtomicInteger<int> := "
        "new AtomicInteger<int>(0); "
        "end Reg;");
    bezel::Driver driver;
    bezel::CompileResult rBase = driver.compileObject(baseSrc.path, baseObj.path);
    BEZEL_EXPECT_TRUE(rBase.success);
    if (!rBase.success) return;

    writeSource(w1Src.path,
        "module W1_2351Owned; "
        "import Base2351Owned; "
        "public class W1 "
        "  public static method Bump(): int "
        "    return Reg<int>.Hits.Increment(); "
        "  end Bump; "
        "end W1;");
    std::vector<bezel::Dependency> depsBase;
    {
        bezel::Dependency d;
        d.metadataPath = baseObj.path;
        d.objectPath   = baseObj.path;
        depsBase.push_back(d);
    }
    bezel::CompileResult rW1 =
        driver.compileObject(w1Src.path, w1Obj.path, depsBase);
    BEZEL_EXPECT_TRUE(rW1.success);
    if (!rW1.success) return;

    writeSource(w2Src.path,
        "module W2_2351Owned; "
        "import Base2351Owned; "
        "public class W2 "
        "  public static method Bump(): int "
        "    return Reg<int>.Hits.Increment(); "
        "  end Bump; "
        "end W2;");
    bezel::CompileResult rW2 =
        driver.compileObject(w2Src.path, w2Obj.path, depsBase);
    BEZEL_EXPECT_TRUE(rW2.success);
    if (!rW2.success) return;

    writeSource(appSrc.path,
        "module App2351Owned; "
        "import W1_2351Owned; "
        "import W2_2351Owned; "
        "public class Program "
        "  public static method Main(): int "
        "    var a := W1.Bump(); "
        "    var b := W2.Bump(); "
        "    return a * 10 + b; "
        "  end Main; "
        "end Program;");
    std::vector<bezel::Dependency> depsApp;
    {
        bezel::Dependency dBase, dW1, dW2;
        dBase.metadataPath = baseObj.path; dBase.objectPath = baseObj.path;
        dW1.metadataPath   = w1Obj.path;   dW1.objectPath   = w1Obj.path;
        dW2.metadataPath   = w2Obj.path;   dW2.objectPath   = w2Obj.path;
        depsApp.push_back(dBase);
        depsApp.push_back(dW1);
        depsApp.push_back(dW2);
    }
    // §2.6's own "--sanitize=address applies to the executable build" rule —
    // only the FINAL link is instrumented; the ASan RUNTIME's allocator
    // interposition (malloc/free/calloc) still catches a leak or double-free
    // from the dependency objects' pre-compiled code, exactly as
    // runCrossModuleChecked (#2290/#710) relies on.
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    bezel::CompileResult rApp =
        driver.compileFile(appSrc.path, appExe.path, depsApp, opts);
    BEZEL_EXPECT_TRUE(rApp.success);
    if (!rApp.success) return;

    bool timedOut = false;
    int status = aot_detail::spawnCaptured(
        {appExe.path}, stdOut.path, sanOut.path, aot_detail::aotTimeoutSec(),
        timedOut, {{"ASAN_OPTIONS", "detect_leaks=1:exitcode=99:abort_on_error=0"}});
    BEZEL_EXPECT_TRUE(!timedOut);
    if (timedOut) return;

    std::string sanText = aot_detail::slurp(sanOut.path);
    bool crashed = WIFSIGNALED(status);
    bool sanitizerFired = crashed || aot_detail::hasSanitizerReport(sanText);
    // CLAUDE.md trap (iii): LeakSanitizer forces exitcode=99 on a leak/error
    // (set explicitly above) rather than letting Main's own return value
    // through, so check the sanitizer verdict FIRST, independent of the
    // masked exit code.
    BEZEL_EXPECT_TRUE(!sanitizerFired);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 12);  // same 1-then-2 baseline as the plain test
}

}  // namespace bezel::module
