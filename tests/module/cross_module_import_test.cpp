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
    std::string stem = "bezel_xm_vdisp_" + std::to_string(std::rand());
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
    std::string stem = "bezel_xm_dispose_" + std::to_string(std::rand());
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
    std::string stem = "bezel_xm_nfinit_" + std::to_string(std::rand());
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
    std::string stem = "bezel_xm_enum_" + std::to_string(std::rand());
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
    std::string stem = "bezel_xm_enumeq_" + std::to_string(std::rand());
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
    std::string stem = "bezel_xm_enumcase_" + std::to_string(std::rand());
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
        "    return case c "
        "      Color.Red => 11, "
        "      Color.Green => 22, "
        "      Color.Blue => 33, "
        "    end; "
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
    std::string stem = "bezel_xm_enumimp_" + std::to_string(std::rand());
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
BEZEL_TEST(cross_module_import, imported_generic_new_rejected_spec_14) {
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
    BEZEL_EXPECT_TRUE(r->hadErrors);
    // The diagnostic must name the imported-generic-construction limitation, so
    // the failure is an honest compile error rather than a cryptic linker error.
    bool sawLimitation = false;
    for (const Diagnostic& d : r->diags.diagnostics()) {
        if (d.message.find("imported generic") != std::string::npos &&
            d.message.find("\xC2\xA7""14") != std::string::npos)
            sawLimitation = true;
    }
    BEZEL_EXPECT_TRUE(sawLimitation);
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
// dangling `bz$<Type>$init` reference is emitted.
BEZEL_TEST(cross_module_import, imported_generic_new_native_clean_failure_spec_14) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_xm_genbox_" + std::to_string(std::rand());
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
    // The library compiles (a generic template is emittable single-module).
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    // The importer must FAIL CLEANLY (a compile error from sema), not link.
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(!rb.success);
    // No executable should have been produced (the build aborted before link).
    BEZEL_EXPECT_TRUE(!std::filesystem::exists(bExe.path));
}

// §14 / §2.5 (FINDING [2]): an imported generic STATIC method with type
// inference is UNAFFECTED by the imported-generic-`new` carve-out — only
// generic-TYPE construction is rejected. `Util.Echo<T>(x)` called as
// `Util.Echo(42)` (T inferred = int) compiles, links, and returns 42.
BEZEL_TEST(cross_module_import, imported_generic_static_method_still_ok_spec_14) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_xm_genecho_" + std::to_string(std::rand());
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
    std::string stem = "bezel_xm_derive_" + std::to_string(std::rand());
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
    std::string stem = "bezel_xm_derivefields_" + std::to_string(std::rand());
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
    std::string stem = "bezel_xm_derivedisp_" + std::to_string(std::rand());
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
    std::string stem = "bezel_xm_iface1_" + std::to_string(std::rand());
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
    std::string stem = "bezel_xm_iface2_" + std::to_string(std::rand());
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
    std::string stem = "bezel_xm_iface3_" + std::to_string(std::rand());
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
    std::string stem = "bezel_428_grbs_" + std::to_string(std::rand());
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
    std::string stem = "bezel_428_grbi_" + std::to_string(std::rand());
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
    std::string stem = "bezel_428_grpair_" + std::to_string(std::rand());
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
    std::string stem = "bezel_428_grwith_" + std::to_string(std::rand());
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
        "    var b2 := b with value := 99 end; "
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
    std::string stem = "bezel_428_grfield_" + std::to_string(std::rand());
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
    std::string stem = "bezel_428_grdbl_" + std::to_string(std::rand());
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

// (8) NEGATIVE — §14 deferred: imported generic CLASS with field initializer is
// still rejected (needs importer-side vtable + disposer; §28.3 obligation).
// §14.1 / §22.8: Pins the remaining gate boundary.
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
        "    return h.n; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r->hadErrors);
    bool sawLimitation = false;
    for (const Diagnostic& d : r->diags.diagnostics()) {
        if (d.message.find("imported generic") != std::string::npos &&
            d.message.find("\xC2\xA7""14") != std::string::npos)
            sawLimitation = true;
    }
    BEZEL_EXPECT_TRUE(sawLimitation);
}

// (9) NEGATIVE — §14 deferred: imported generic CLASS with an explicit constructor
// BODY must still produce the clean §14 diagnostic.
BEZEL_TEST(cross_module_import, imported_generic_ctor_body_still_diagnosed_spec_14) {
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
        "    using var w := new Wrapper<int>(3); "  // ctor BODY not yet supported
        "    return 0; "
        "  end Main; "
        "end Program;");
    // The construction must be rejected (a diagnostic), not silently miscompiled.
    BEZEL_EXPECT_TRUE(r->hadErrors);
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
    std::string stem = "bezel_428_grmath_" + std::to_string(std::rand());
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
    std::string stem = "bezel_428_grstrlen_" + std::to_string(std::rand());
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
    std::string stem = "bezel_428_grpair2_" + std::to_string(std::rand());
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
        "    var b2 := b with Value := \"ok\" end; "
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

}  // namespace bezel::module
