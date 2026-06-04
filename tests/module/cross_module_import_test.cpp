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
#include "test_harness.hpp"

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
    return buildModuleMetadata(ctx);
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

}  // namespace

// §2.5 / §2.6 step 4: A-imports-B native build. A exports `Square`/`Add`; B
// imports A and computes `Add(Square(6), 6)` = 42; link A's object into B's
// executable; run → 42.
BEZEL_TEST(cross_module_import, a_imports_b_native_build_spec_2_6) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_inc3_" + std::to_string(std::rand());
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

}  // namespace bezel::module
