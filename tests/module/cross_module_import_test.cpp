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

// §11.7 (FINDING [6]): a `sealed abstract` (closed abstract) base may be
// inherited only *within its declaring module*. The importer App imports a
// `sealed abstract` Shape from Lib and declares a local `class Local : Shape` —
// a CROSS-module derivation of the closed-abstract base — which the compiler
// must reject ("the compiler rejects subclasses in other modules", §11.7). The
// base arrives flagged as imported, so the inheritance gate distinguishes it
// from a same-module base.
BEZEL_TEST(cross_module_import, sealed_abstract_cross_module_subclass_rejected_spec_11_7) {
    ModuleMetadata dep = buildMetaFor(
        "module ShapeLib; "
        "public sealed abstract class Shape "
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
BEZEL_TEST(cross_module_import, imported_sealed_abstract_use_without_subclass_ok_spec_11_7) {
    ModuleMetadata dep = buildMetaFor(
        "module ShapeLib; "
        "public sealed abstract class Shape "
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

// §2.5 / §2.6 + §22.12 (issue #328 FINDING [3] BUG A): a DIRECT read of an
// imported static `const` must observe the library's value. A §22.12 const
// initializer "must be a compile-time-constant expression", so the library
// emits NO storage symbol for it; the importer FOLDS the value (carried in the
// dependency metadata) at the read site. Before the fix this returned
// non-deterministic garbage (an uninitialized read); after, it returns 7.
BEZEL_TEST(cross_module_import, imported_const_read_spec_22_12) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_inc3_const_" + std::to_string(std::rand());
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
    std::string stem = "bezel_inc3_field_" + std::to_string(std::rand());
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
    std::string stem = "bezel_inc4_ctorA_" + std::to_string(std::rand());
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
    std::string stem = "bezel_inc4_ctorB_" + std::to_string(std::rand());
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
    std::string stem = "bezel_inc4_fieldinit_" + std::to_string(std::rand());
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

}  // namespace bezel::module
