// Cross-module property getter dispatch (issue #503, §20 / §2.5 / §22.9).
//
// Bug: an imported class's property getter returns 0 cross-module instead of
// the real value.  The root cause is three-fold:
//
//   1. ModuleLoader::convertMember returned nullptr for MetaMemberKind::Property,
//      so no PropertySymbol was reconstructed → lookup returned null → codegen
//      lowered the read to 0.
//   2. ExprTypecheck_Calls did not set the expression type for an imported
//      PropertySymbol (only Field/Const/EnumConstant were handled in the
//      early-return imported block), so the member-access expression was untyped.
//   3. Codegen: (a) for virtual/override properties, lowerVirtualPropertyGet
//      bailed out at `if (!pd) return nullptr` (pd == null for an imported
//      symbol); (b) for regular non-virtual properties, no externally-linkable
//      getter function was emitted by the exporter, so the importer had no
//      symbol to call.
//
// The fix: reconstruct PropertySymbol in the loader; extend sema type attachment;
// emit ExternalLinkage getter stubs in the exporter for non-virtual public
// properties; fix the `!pd` guard in lowerVirtualPropertyGet for the virtual case.

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

// Reuse the same two helpers as cross_module_import_test.cpp.
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

}  // namespace

// --- sema-level: loader reconstructs PropertySymbol (§20 / §2.5) -------------

// §2.5 / §22.9: a Property member exported by a module must be reconstructed as
// a PropertySymbol in the loader so that a cross-module `h.N` resolves correctly.
// Before the fix, MetaMemberKind::Property hit `default: return nullptr` in
// convertMember, leaving the type's member list without any PropertySymbol.
BEZEL_TEST(xmod_property_503, loader_reconstructs_property_symbol_spec_2_5) {
    ModuleMetadata dep = buildMetaFor(
        "module HLib; "
        "public class Holder "
        "  public _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property N: int => _n; "
        "  destructor end destructor; "
        "end Holder;");
    DiagnosticEngine diags;
    sema::SemaContext ctx(diags);
    BEZEL_EXPECT_TRUE(loadMetadata(dep, ctx));
    const std::vector<sema::TypeSymbol*>* exported = ctx.importedTypesOf("HLib");
    BEZEL_EXPECT_TRUE(exported != nullptr);
    if (!exported) return;
    sema::TypeSymbol* holder = nullptr;
    for (sema::TypeSymbol* t : *exported)
        if (t->name == "Holder") holder = t;
    BEZEL_EXPECT_TRUE(holder != nullptr);
    if (!holder) return;
    // The PropertySymbol for N must appear in the member list.
    sema::PropertySymbol* propN = nullptr;
    for (sema::Symbol* m : holder->members)
        if (auto* ps = dynamic_cast<sema::PropertySymbol*>(m))
            if (ps->name == "N") propN = ps;
    BEZEL_EXPECT_TRUE(propN != nullptr);
}

// §2.5 / §22.9: an imported property's type (carried in MetaMember::type) must be
// stored in the importedMemberType side table so that sema and codegen can read
// the property's resolved return type (there is no ast::Type node on the imported
// symbol to key the normal resolvedTypes_ table).
BEZEL_TEST(xmod_property_503, imported_property_type_stored_spec_2_5) {
    ModuleMetadata dep = buildMetaFor(
        "module HLib; "
        "public class Holder "
        "  public _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property N: int => _n; "
        "  destructor end destructor; "
        "end Holder;");
    DiagnosticEngine diags;
    sema::SemaContext ctx(diags);
    BEZEL_EXPECT_TRUE(loadMetadata(dep, ctx));
    const std::vector<sema::TypeSymbol*>* exported = ctx.importedTypesOf("HLib");
    if (!exported) return;
    sema::TypeSymbol* holder = nullptr;
    for (sema::TypeSymbol* t : *exported) if (t->name == "Holder") holder = t;
    if (!holder) return;
    sema::PropertySymbol* propN = nullptr;
    for (sema::Symbol* m : holder->members)
        if (auto* ps = dynamic_cast<sema::PropertySymbol*>(m))
            if (ps->name == "N") propN = ps;
    BEZEL_EXPECT_TRUE(propN != nullptr);
    if (!propN) return;
    // The resolved type must be int (Primitive::Int).
    sema::TypeRef* ty = ctx.importedMemberType(propN);
    BEZEL_EXPECT_TRUE(ty != nullptr);
    if (ty) {
        BEZEL_EXPECT_TRUE(ty->kind == sema::TypeRef::Kind::Primitive);
        BEZEL_EXPECT_TRUE(ty->primitive == sema::PrimitiveType::Int);
    }
}

// §2.5 / §22.9: importing a module with a property and accessing it type-checks
// clean (no diagnostics). Before the fix the symbol was absent, so member lookup
// found nothing, and while no error was emitted (imported type has null decl →
// not a "known-complete" type), the result was untyped and returned 0 at runtime.
BEZEL_TEST(xmod_property_503, imported_property_access_typechecks_clean_spec_2_5) {
    ModuleMetadata dep = buildMetaFor(
        "module HLib; "
        "public class Holder "
        "  public _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property N: int => _n; "
        "  destructor end destructor; "
        "end Holder;");
    auto r = runImporter(dep,
        "module App; "
        "import HLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(42); "
        "    return h.N; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// --- CONTROL: same-module property getter still works (regression guard) ------

// §22.9: a same-module (non-imported) arrow-getter property must still return the
// correct value after the cross-module fix.  If the exporter-side change broke the
// local path, this will catch it.
BEZEL_TEST(xmod_property_503, same_module_property_getter_still_works_spec_22_9) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_503_samemod_" + std::to_string(std::rand());
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aExe((dir / (stem + "_a")).string());

    writeSource(aSrc.path,
        "module App; "
        "public class Holder "
        "  public _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property N: int => _n; "
        "  destructor end destructor; "
        "end Holder; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(42); "
        "    return h.N; "
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

// --- CONTROL: imported method and direct field read still work ----------------

// §2.5 / §22.6: a cross-module METHOD call (the pre-existing working path) must
// still work after the property fix.  Regression guard for any refactoring that
// might touch the method-declaration path.
BEZEL_TEST(xmod_property_503, imported_method_still_works_control_spec_2_5) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_503_meth_" + std::to_string(std::rand());
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module Lib; "
        "public class Holder "
        "  public _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method Get(): int => _n; "
        "  destructor end destructor; "
        "end Holder;");
    writeSource(bSrc.path,
        "module App; "
        "import Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(42); "
        "    return h.Get(); "
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

// §2.5 / §11: a direct FIELD read on an imported class must still work (the
// field path has its own codegen slot and must not be disturbed by the fix).
BEZEL_TEST(xmod_property_503, imported_field_read_still_works_control_spec_2_5) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_503_fld_" + std::to_string(std::rand());
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module Lib; "
        "public class Holder "
        "  public _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  destructor end destructor; "
        "end Holder;");
    writeSource(bSrc.path,
        "module App; "
        "import Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(99); "
        "    return h._n; "
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

// --- End-to-end: regular (non-virtual) imported property getter --------------

// §2.5 / §2.6 / §22.9 (issue #503 headline): a non-virtual property getter on an
// imported class must return the real value, not 0.  `Holder` is defined in A
// with `public property N: int => _n;`; B imports A and evaluates `h.N` where `h`
// was constructed with n=42.  Before the fix, the getter was never called and the
// result was 0.  After: the exporter emits bz$Holder$N$propget with external
// linkage; the importer calls it; the linked result is 42.
BEZEL_TEST(xmod_property_503, imported_regular_property_getter_returns_value_spec_22_9) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_503_prop_" + std::to_string(std::rand());
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module HLib; "
        "public class Holder "
        "  public _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property N: int => _n; "
        "  destructor end destructor; "
        "end Holder;");
    writeSource(bSrc.path,
        "module App; "
        "import HLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(42); "
        "    return h.N; "  // imported property getter — must return 42, not 0
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

// §22.9 (issue #503): block-form getter (`get => do … end;`) works cross-module.
// The exporter emits the getter with external linkage; the importer calls it.
BEZEL_TEST(xmod_property_503, imported_block_getter_property_returns_value_spec_22_9) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_503_blkget_" + std::to_string(std::rand());
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module BLib; "
        "public class Box "
        "  public _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public property V: int "
        "    get => do return _v; end; "
        "  end V; "
        "  destructor end destructor; "
        "end Box;");
    writeSource(bSrc.path,
        "module App; "
        "import BLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var b := new Box(77); "
        "    return b.V; "  // block-form getter cross-module
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
    BEZEL_EXPECT_EQ(code, 77);
}

// --- End-to-end: virtual/override imported property getter -------------------

// §2.5 / §2.6 / §12.1 / §22.9 / §23 (issue #503): a VIRTUAL property getter on
// an imported class dispatches correctly through the vtable.  `Animal` in A has
// `virtual property Sound: int => 1`; `Dog` overrides it with `=> 2`; B calls
// `a.Sound` through an `Animal`-typed parameter.  Before the fix:
//   (a) PropertySymbol was absent from the imported type, and
//   (b) lowerVirtualPropertyGet bailed at `if (!pd) return nullptr`.
// After: vtable slot dispatch → 2.
BEZEL_TEST(xmod_property_503, imported_virtual_property_dispatch_spec_12_22_9) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_503_vprop_" + std::to_string(std::rand());
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module VPLib; "
        "public class Animal "
        "  public virtual property Sound: int => 1; "
        "  destructor end destructor; "
        "end Animal; "
        "public class Dog : Animal "
        "  public override property Sound: int => 2; "
        "  destructor end destructor; "
        "end Dog;");
    writeSource(bSrc.path,
        "module App; "
        "import VPLib; "
        "public class Program "
        "  public static method Tone(a: Animal): int => a.Sound; "
        "  public static method Main(): int "
        "    using var d := new Dog(); "
        "    return Tone(d); "  // virtual property dispatch → Dog.Sound == 2
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
    BEZEL_EXPECT_EQ(code, 2);
}

// §2.5 / §2.6 / §12.1 / §23 (issue #503): an ABSTRACT property on a base class
// imported by the importer's LOCAL subclass.  `IShape` declares `abstract
// property Area: int`; local `Circle` overrides it with `=> 7`.  An `IShape`-
// typed variable holds a `Circle` and reads `.Area` → 7.
BEZEL_TEST(xmod_property_503, abstract_property_dispatch_through_base_spec_12_23) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_503_absprop_" + std::to_string(std::rand());
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module ShapeLib; "
        "public abstract class Shape "
        "  public abstract property Area: int; "
        "  destructor end destructor; "
        "end Shape;");
    writeSource(bSrc.path,
        "module App; "
        "import ShapeLib; "
        "public class Circle : Shape "
        "  public override property Area: int => 7; "
        "  destructor end destructor; "
        "end Circle; "
        "public class Program "
        "  public static method GetArea(s: Shape): int => s.Area; "
        "  public static method Main(): int "
        "    using var c := new Circle(); "
        "    return GetArea(c); "  // abstract property dispatch → Circle.Area == 7
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

}  // namespace bezel::module
