// Regression test for #2287 — imports of GENERIC classes must reject unknown
// member access with a proper error, not silently fall through to codegen's
// permissive-acceptance backstop (which would degrade the slot to zero-init and
// produce a garbage runtime value).
//
// BEFORE (broken): `bezelc --import lib.o app.bzl` compiles without error,
// `./app` returns 0 (zero-initialized, garbage). `nm app` shows NO call symbol
// for the member — the call was silently dropped.
//
// AFTER (fixed): `bezelc --import lib.o app.bzl` emits:
//   app.bzl:N:M: no member `TotallyBogusName` on type `Box` (§11.3, §12.1)
//
// AUTHORITY.
//
// spec §2.5 (v0.298), "Import": imported symbols are part of the program;
// spec §11.3 / §12.1 (member lookup): a member not in the type's member set
// is a compile error.
//
// ROOT CAUSE.
//
// `importedKnownCompleteUserType` in ExprTypecheck_Calls.cpp was guarded by
// `genericParams.empty()`, excluding imported generic types from the
// known-complete-user-type path. This was a hedge against incomplete metadata
// (per issue #795's closing comment). The §2.5 metadata for an imported
// generic class carries every exported member, so the hedge is no longer
// warranted — removing the guard lets the standard "no member" error path
// fire correctly for imported generic types.
//
// TEST STRATEGY.
//
// Uses the sema-level harness from cross_module_import_test.cpp: build metadata
// for a generic class, run the importer, verify errors are emitted for unknown
// member access. Does NOT require a full compile/link/run cycle.

#include "module/ModuleLoader.hpp"

#include <string>

#include "ast/Node.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
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

// Build the ModuleMetadata for `source` (a dependency module).
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
    return buildModuleMetadata(ctx, source);
}

// Run the importer against a dependency's metadata.
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
    sema::collectSymbols(*r->sf, *r->ctx);
    loadMetadata(dep, *r->ctx);
    sema::resolveTypes(*r->sf, *r->ctx);
    sema::typecheckExpressions(*r->sf, *r->ctx);
    sema::checkDefiniteAssignment(*r->sf, *r->ctx);
    r->hadErrors = r->diags.hasErrors();
    return r;
}

}  // namespace

// fix #2287 (§2.5 / §11.3 / §12.1, v0.298): an unknown member access on an
// imported GENERIC class must emit "no member" error, not silently fall through.
BEZEL_TEST(xmod_imported_generic_unknown_member_2287,
           unknown_member_on_imported_generic_rejected) {
    // lib.bzl — a generic class with one known member
    ModuleMetadata dep = buildMetaFor(
        "module IfaceLib; "
        "public class Box<T> "
        "  public _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "end Box;");
    // app.bzl — calls TotallyBogusName() on Box<int>, which does not exist
    auto r = runImporter(
        dep,
        "module App; "
        "import IfaceLib; "
        "class Program "
        "  public static method Main(): int "
        "    var b := new Box<int>(9); "
        "    var r := b.TotallyBogusName(); "  // unknown member — must error
        "    return r; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r->hadErrors);
}

// fix #2287: same shape with a KNOWN member should compile clean.
BEZEL_TEST(xmod_imported_generic_unknown_member_2287,
           known_member_on_imported_generic_compiles_clean) {
    ModuleMetadata dep = buildMetaFor(
        "module IfaceLib; "
        "public class Box<T> "
        "  public _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "end Box;");
    auto r = runImporter(
        dep,
        "module App; "
        "import IfaceLib; "
        "class Program "
        "  public static method Main(): int "
        "    var b := new Box<int>(9); "
        "    return b._n; "  // known member — should compile clean
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

}  // namespace bezel::module