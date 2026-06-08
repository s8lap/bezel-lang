// Cross-module positional record `=` (issue #501, §9.2) and nested-type export
// (issue #502, §11.3 / §2.5).
//
// Bug #501: an imported positional record (e.g. `record Point(x:int, y:int)`)
//   loses its "positional / auto-equality-bearing" flag in the importer, so
//   `p = q` is rejected with "not auto-equality-bearing (§9.2)". The fix: carry
//   the hasPositionalFields flag through module metadata and set it on the
//   reconstructed TypeSymbol in ModuleLoader so the §9.2 check in
//   ExprTypecheck_Exprs.cpp reads it from TypeSymbol directly (not from the
//   absent ast::TypeDecl).
//
// Bug #502a: an imported class's PUBLIC NESTED TYPES are not exported by
//   ModuleMetadataBuilder and not reconstructed by ModuleLoader. So
//   `new Outer.Inner()` from an importer produces "type Outer has no nested
//   type Inner §11.3". The fix: add MetaMemberKind::NestedType to the metadata
//   schema, emit nested types from the builder, reconstruct NestedTypeSymbol +
//   inner TypeSymbol in the loader.
//
// Bug #502b: an imported nested type's STATIC METHOD returns 0 instead of the
//   real value. This is a mangled-name mismatch IF the nested type IS exported
//   (which requires #502a first). Since the local exporter uses
//   `bz$<Inner>$<Method>` (with the INNER class name), the importer's
//   declareImportedFunctions likewise uses functionName(ms) → `bz$<Inner>$<Method>`,
//   so the names match once the type is exported. This test validates the end-
//   to-end behaviour.

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

// ─── shared helpers (mirrors xmod_property_503_test.cpp) ─────────────────────

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
    if (!r->sf) { r->hadErrors = true; return r; }
    r->ctx = std::make_unique<sema::SemaContext>(r->diags);
    sema::collectSymbols(*r->sf, *r->ctx);
    loadMetadata(dep, *r->ctx);
    sema::resolveTypes(*r->sf, *r->ctx);
    sema::typecheckExpressions(*r->sf, *r->ctx);
    sema::checkDefiniteAssignment(*r->sf, *r->ctx);
    r->hadErrors = r->diags.hasErrors();
    return r;
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// ─── §9.2 / §9.1 — CONTROL: same-module positional record `=` ───────────────

// §9.1 / §9.2: a SAME-MODULE positional record `=` returns the correct bool.
// This is the baseline that must not regress after the cross-module fix.
BEZEL_TEST(xmod_record_nested_501_502, same_module_positional_eq_spec_9_1_9_2) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_501_sm_" + std::to_string(std::rand());
    TempFile aSrc((dir / (stem + ".bzl")).string());
    TempFile aExe((dir / (stem + "_a")).string());

    writeSource(aSrc.path,
        "module App; "
        "public record Point(x: int, y: int); "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new Point(1, 2); "
        "    var q := new Point(1, 2); "
        "    return (10 if p = q else 0); "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileFile(aSrc.path, aExe.path, {});
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;
    int status = std::system(aExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 10);
}

// ─── §9.2 — Bug #501: imported positional record `=` should type-check clean ─

// §9.2 / §2.5 (issue #501): importing a module that defines a positional record
// and using `p = q` on it must NOT produce a diagnostic. Before the fix, the
// reconstructed TypeSymbol had no AST node, so the §9.2 positional check in
// ExprTypecheck_Exprs.cpp read a null ast::TypeDecl* and `positional` stayed
// false, triggering the "not auto-equality-bearing" error.
BEZEL_TEST(xmod_record_nested_501_502, imported_positional_eq_typechecks_clean_spec_9_2) {
    ModuleMetadata dep = buildMetaFor(
        "module PLib; "
        "public record Point(x: int, y: int);");
    auto r = runImporter(dep,
        "module App; "
        "import PLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new Point(1, 2); "
        "    var q := new Point(1, 2); "
        "    return (1 if p = q else 0); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// ─── §9.2 / §9.1 — Bug #501: end-to-end (equal case returns non-zero) ────────

// §9.1 / §9.2 / §2.5 (issue #501): an imported positional record's `=` must
// return the correct value when both records have equal fields (exit 7).
BEZEL_TEST(xmod_record_nested_501_502, imported_positional_eq_equal_case_spec_9_2) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_501_eq_" + std::to_string(std::rand());
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module PLib; "
        "public record Point(x: int, y: int);");
    writeSource(bSrc.path,
        "module App; "
        "import PLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new Point(3, 4); "
        "    var q := new Point(3, 4); "
        "    return (7 if p = q else 0); "   // equal → 7
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d; d.metadataPath = aObj.path; d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 7);
}

// §9.1 / §9.2 / §2.5 (issue #501): an imported positional record's `=` must
// return 0 when the records have unequal fields.
BEZEL_TEST(xmod_record_nested_501_502, imported_positional_eq_unequal_case_spec_9_2) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_501_ne_" + std::to_string(std::rand());
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module PLib; "
        "public record Point(x: int, y: int);");
    writeSource(bSrc.path,
        "module App; "
        "import PLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new Point(1, 2); "
        "    var q := new Point(3, 4); "
        "    return (1 if p = q else 0); "   // not equal → 0
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d; d.metadataPath = aObj.path; d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 0);
}

// ─── §9.2 — CONTROL: a body record without user equality still errors cross-mod

// §9.2 / §2.5: a BODY record that is NOT auto-equality-bearing (it has a field
// with an explicit FieldDecl) must STILL produce an error cross-module. The fix
// for #501 must not accidentally allow body-record `=` without an operator.
BEZEL_TEST(xmod_record_nested_501_502, imported_body_record_eq_still_errors_spec_9_2) {
    ModuleMetadata dep = buildMetaFor(
        "module RLib; "
        "public record Box "
        "  public _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "end Box;");
    auto r = runImporter(dep,
        "module App; "
        "import RLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new Box(1); "
        "    using var b := new Box(1); "
        "    return (1 if a = b else 0); "   // must be an error
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r->hadErrors);
}

// ─── §11.3 / §2.5 — CONTROL: same-module nested type + nested static ─────────

// §11.3: same-module `Outer.Inner.Value()` (nested static method) must work.
// This is the baseline that must not regress after #502a.
BEZEL_TEST(xmod_record_nested_501_502, same_module_nested_type_spec_11_3) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_502_sm_" + std::to_string(std::rand());
    TempFile aSrc((dir / (stem + ".bzl")).string());
    TempFile aExe((dir / (stem + "_a")).string());

    writeSource(aSrc.path,
        "module App; "
        "public class Outer "
        "  public class Inner "
        "    public static method Value(): int => 42; "
        "    destructor end destructor; "
        "  end Inner; "
        "  destructor end destructor; "
        "end Outer; "
        "public class Program "
        "  public static method Main(): int "
        "    return Outer.Inner.Value(); "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileFile(aSrc.path, aExe.path, {});
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;
    int status = std::system(aExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 42);
}

// ─── §11.3 / §2.5 — Bug #502a: sema-level: loader reconstructs NestedTypeSymbol

// §11.3 / §2.5 (issue #502a): after loading a module whose public class has a
// public nested type, the importer's context must contain a NestedTypeSymbol for
// that nested type in the outer type's member list — so that `new Outer.Inner()`
// resolves across the module boundary.
BEZEL_TEST(xmod_record_nested_501_502, loader_reconstructs_nested_type_symbol_spec_11_3) {
    ModuleMetadata dep = buildMetaFor(
        "module OLib; "
        "public class Outer "
        "  public class Inner "
        "    public static method Value(): int => 42; "
        "    destructor end destructor; "
        "  end Inner; "
        "  destructor end destructor; "
        "end Outer;");

    DiagnosticEngine diags;
    sema::SemaContext ctx(diags);
    BEZEL_EXPECT_TRUE(loadMetadata(dep, ctx));

    const std::vector<sema::TypeSymbol*>* exported = ctx.importedTypesOf("OLib");
    BEZEL_EXPECT_TRUE(exported != nullptr);
    if (!exported) return;

    sema::TypeSymbol* outer = nullptr;
    for (sema::TypeSymbol* t : *exported)
        if (t->name == "Outer") outer = t;
    BEZEL_EXPECT_TRUE(outer != nullptr);
    if (!outer) return;

    // The NestedTypeSymbol for Inner must be in Outer's member list.
    sema::NestedTypeSymbol* nts = nullptr;
    for (sema::Symbol* m : outer->members) {
        if (auto* n = dynamic_cast<sema::NestedTypeSymbol*>(m)) {
            if (n->name == "Inner") { nts = n; break; }
        }
    }
    BEZEL_EXPECT_TRUE(nts != nullptr);
    if (!nts) return;

    // The inner TypeSymbol must be non-null and carry the Inner class.
    BEZEL_EXPECT_TRUE(nts->type != nullptr);
    if (nts->type) {
        BEZEL_EXPECT_TRUE(nts->type->kind == ast::TypeDeclKind::Class);

        // The inner type's members must include the static method Value.
        sema::MethodSymbol* ms = nullptr;
        for (sema::Symbol* m : nts->type->members) {
            if (auto* meth = dynamic_cast<sema::MethodSymbol*>(m)) {
                if (meth->name == "Value") { ms = meth; break; }
            }
        }
        BEZEL_EXPECT_TRUE(ms != nullptr);
    }
}

// §11.3 / §2.5 (issue #502a): importing a module with a public nested type and
// using `Outer.Inner.Value()` (a nested static method) must type-check clean.
BEZEL_TEST(xmod_record_nested_501_502, imported_nested_static_typechecks_clean_spec_11_3) {
    ModuleMetadata dep = buildMetaFor(
        "module OLib; "
        "public class Outer "
        "  public class Inner "
        "    public static method Value(): int => 42; "
        "    destructor end destructor; "
        "  end Inner; "
        "  destructor end destructor; "
        "end Outer;");
    auto r = runImporter(dep,
        "module App; "
        "import OLib; "
        "public class Program "
        "  public static method Main(): int "
        "    return Outer.Inner.Value(); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// ─── §11.3 / §2.5 — Bug #502b: end-to-end: nested static returns real value ──

// §11.3 / §2.5 (issue #502b): a nested static method called cross-module must
// return the actual value, not 0. `Outer.Inner.Value()` returns 42 from the
// exporter's method body. The importer calls it via declareImportedFunctions
// using `bz$Inner$Value`, which matches the exporter's symbol name.
BEZEL_TEST(xmod_record_nested_501_502, imported_nested_static_returns_real_value_spec_11_3) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_502b_stat_" + std::to_string(std::rand());
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module OLib; "
        "public class Outer "
        "  public class Inner "
        "    public static method Value(): int => 42; "
        "    destructor end destructor; "
        "  end Inner; "
        "  destructor end destructor; "
        "end Outer;");
    writeSource(bSrc.path,
        "module App; "
        "import OLib; "
        "public class Program "
        "  public static method Main(): int "
        "    return Outer.Inner.Value(); "   // nested static cross-module
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d; d.metadataPath = aObj.path; d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 42);
}

// §11.3 / §2.5 (issue #502a): `new Outer.Inner(v)` end-to-end: construct an
// Inner object from the importer, call a method on it, and return the result.
BEZEL_TEST(xmod_record_nested_501_502, imported_nested_type_construction_e2e_spec_11_3) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_502a_new_" + std::to_string(std::rand());
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module OLib; "
        "public class Outer "
        "  public class Inner "
        "    public _v: int; "
        "    public constructor(v: int) _v := v; end constructor; "
        "    public method Get(): int => _v; "
        "    destructor end destructor; "
        "  end Inner; "
        "  destructor end destructor; "
        "end Outer;");
    writeSource(bSrc.path,
        "module App; "
        "import OLib; "
        "public class Program "
        "  public static method Make(): int "
        "    using var i := new Outer.Inner(55); "   // §11.3 cross-module
        "    return i.Get(); "
        "  end Make; "
        "  public static method Main(): int => Make(); "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d; d.metadataPath = aObj.path; d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 55);
}

// ─── CONTROL: imported top-level class is not disturbed by the nested-type fix ─

// §2.5 / §11: importing a top-level class and calling a method on it must still
// work after the metadata changes (regression guard for #502a changes to the
// builder and loader).
BEZEL_TEST(xmod_record_nested_501_502, imported_toplevel_method_still_works_spec_2_5) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_502_ctrl_" + std::to_string(std::rand());
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module Ctrl; "
        "public class Widget "
        "  public _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method Get(): int => _n; "
        "  destructor end destructor; "
        "end Widget;");
    writeSource(bSrc.path,
        "module App; "
        "import Ctrl; "
        "public class Program "
        "  public static method Main(): int "
        "    using var w := new Widget(99); "
        "    return w.Get(); "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d; d.metadataPath = aObj.path; d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 99);
}

}  // namespace
}  // namespace bezel::module
