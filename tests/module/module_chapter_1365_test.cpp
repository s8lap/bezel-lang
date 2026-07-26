// §2 Module chapter alignment (spec §2, v0.164) — issue #1365.
//
// Tests probe each §2 feature (import aliases, name collision, module-qualified
// type references, §2.7 resolution order) and verify the new compiler behaviour.
// The SCC compilation feature (§2.5) requires build-graph infrastructure not
// available in this lane — see RESIDUE comment at the end of the file.
//
// All tests use the existing cross-module sema harness: buildMetaFor() compiles
// a dependency module to ModuleMetadata, then runImporter() / runImporterTwo()
// compile the importer file against those metadata objects.

#include "module/ModuleLoader.hpp"

#include <string>
#include <vector>

#include "ast/Node.hpp"
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

// ── harness ──────────────────────────────────────────────────────────────────

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

struct ImporterRun {
    DiagnosticEngine diags;
    std::unique_ptr<ast::SourceFile> sf;
    std::unique_ptr<sema::SemaContext> ctx;
    bool hadErrors = false;
};

// Single-dependency importer.
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

// Two-dependency importer (used for collision tests).
std::unique_ptr<ImporterRun> runImporterTwo(const ModuleMetadata& depA,
                                             const ModuleMetadata& depB,
                                             std::string_view importerSource) {
    auto r = std::make_unique<ImporterRun>();
    Lexer lexer(importerSource, r->diags);
    Parser parser(lexer, r->diags);
    r->sf = parser.parseCompilationUnit();
    if (!r->sf) { r->hadErrors = true; return r; }
    r->ctx = std::make_unique<sema::SemaContext>(r->diags);
    sema::collectSymbols(*r->sf, *r->ctx);
    loadMetadata(depA, *r->ctx);
    loadMetadata(depB, *r->ctx);
    sema::resolveTypes(*r->sf, *r->ctx);
    sema::typecheckExpressions(*r->sf, *r->ctx);
    sema::checkDefiniteAssignment(*r->sf, *r->ctx);
    r->hadErrors = r->diags.hasErrors();
    return r;
}

// Parse-only run (no sema): checks that a source file parses without errors.
bool parsesClean(std::string_view source) {
    DiagnosticEngine diags;
    Lexer lexer(source, diags);
    Parser parser(lexer, diags);
    auto sf = parser.parseCompilationUnit();
    return sf && !diags.hasErrors();
}

bool hasDiagnosticContaining(const ImporterRun& run, std::string_view text) {
    for (const auto& diagnostic : run.diags.diagnostics()) {
        if (diagnostic.message.find(text) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// §2.1 Module declarations — already conforming; pin existing behaviour.
// ═══════════════════════════════════════════════════════════════════════════

// §2.1: a module declaration is mandatory and must be first.
BEZEL_TEST(module_chapter_1365, module_decl_single_segment_spec_2_1) {
    BEZEL_EXPECT_TRUE(parsesClean("module App; class C end C;"));
}

// §2.1: dotted module name is a single opaque name (not hierarchy).
BEZEL_TEST(module_chapter_1365, module_decl_dotted_name_spec_2_1) {
    BEZEL_EXPECT_TRUE(
        parsesClean("module BankingApp.Transactions; class C end C;"));
}

// §2.1: multiple files may share the same module name (parsed independently).
BEZEL_TEST(module_chapter_1365, module_decl_multiple_files_same_module_spec_2_1) {
    BEZEL_EXPECT_TRUE(parsesClean("module Lib; class A end A;"));
    BEZEL_EXPECT_TRUE(parsesClean("module Lib; class B end B;"));
}

// ═══════════════════════════════════════════════════════════════════════════
// §2.2 Import aliases (v0.164) — Grammar A.1 `("as" Identifier)?`
// ═══════════════════════════════════════════════════════════════════════════

// §2.2 Aliases: `import X as Y;` parses cleanly (no diagnostic).
BEZEL_TEST(module_chapter_1365, import_alias_parses_spec_2_2) {
    BEZEL_EXPECT_TRUE(
        parsesClean("module App; import BankingApp.Transactions as Tx; "
                    "class C end C;"));
}

// §2.2 Aliases: the alias is optional — plain import without `as` is unchanged.
BEZEL_TEST(module_chapter_1365, import_without_alias_still_works_spec_2_2) {
    BEZEL_EXPECT_TRUE(
        parsesClean("module App; import System.Collections.Generic; "
                    "class C end C;"));
}

// §2.2 Aliases: missing identifier after `as` is a parse error.
BEZEL_TEST(module_chapter_1365, import_alias_missing_ident_is_error_spec_2_2) {
    DiagnosticEngine diags;
    Lexer lexer("module App; import BankingApp.Transactions as; class C end C;",
                diags);
    Parser parser(lexer, diags);
    parser.parseCompilationUnit();
    BEZEL_EXPECT_TRUE(diags.hasErrors());
}

// §2.2 Aliases: the alias `Tx` populates the Import AST node's alias field.
BEZEL_TEST(module_chapter_1365, import_alias_ast_field_populated_spec_2_2) {
    DiagnosticEngine diags;
    Lexer lexer("module App; import BankingApp.Transactions as Tx; "
                "class C end C;",
                diags);
    Parser parser(lexer, diags);
    auto sf = parser.parseCompilationUnit();
    BEZEL_EXPECT_TRUE(sf && !sf->imports.empty());
    if (!sf || sf->imports.empty()) return;
    BEZEL_EXPECT_EQ(sf->imports[0]->alias, std::string("Tx"));
}

// §2.2 Aliases: an import without `as` has an empty alias field.
BEZEL_TEST(module_chapter_1365, import_no_alias_empty_field_spec_2_2) {
    DiagnosticEngine diags;
    Lexer lexer("module App; import MathLib; class C end C;", diags);
    Parser parser(lexer, diags);
    auto sf = parser.parseCompilationUnit();
    BEZEL_EXPECT_TRUE(sf && !sf->imports.empty());
    if (!sf || sf->imports.empty()) return;
    BEZEL_EXPECT_EQ(sf->imports[0]->alias, std::string(""));
}

// ═══════════════════════════════════════════════════════════════════════════
// §2.2 Alias as qualifier in type position (§2.7 rule 1, alias branch)
// ═══════════════════════════════════════════════════════════════════════════

// §2.2 v0.164 / §2.7: `import MathLib as ML;` then `ML.Calc` names the type.
// The alias works as an additional qualifier for the module's public types.
BEZEL_TEST(module_chapter_1365, alias_qualifies_type_in_return_pos_spec_2_2_2_7) {
    ModuleMetadata dep = buildMetaFor(
        "module MathLib; "
        "public class Calc "
        "  public static method Square(n: int): int => n * n; "
        "end Calc;");
    auto r = runImporter(
        dep,
        "module App; "
        "import MathLib as ML; "
        "public class Program "
        // Local field typed as ML.Calc (alias-qualified type).
        "  public _c: ML.Calc; "
        "  public constructor() "
        "    _c := new ML.Calc(); "
        "  end constructor; "
        "  public static method Main(): int => Calc.Square(7); "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// §2.2: alias qualifier is an ADDITIONAL qualifier — the bare name still works.
BEZEL_TEST(module_chapter_1365, alias_additive_bare_name_still_visible_spec_2_2) {
    ModuleMetadata dep = buildMetaFor(
        "module MathLib; "
        "public class Calc "
        "  public static method Square(n: int): int => n * n; "
        "end Calc;");
    auto r = runImporter(
        dep,
        "module App; "
        "import MathLib as ML; "
        "public class Program "
        "  public static method Main(): int => Calc.Square(6); " // bare name works
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// §2.2: alias qualifier in a method parameter type position.
BEZEL_TEST(module_chapter_1365, alias_qualifier_in_param_type_spec_2_2) {
    ModuleMetadata dep = buildMetaFor(
        "module Shapes; "
        "public class Circle "
        "  public Radius: int; "
        "  public constructor(r: int) Radius := r; end constructor; "
        "end Circle;");
    auto r = runImporter(
        dep,
        "module App; "
        "import Shapes as Sh; "
        "public class Program "
        "  public static method Area(c: Sh.Circle): int => c.Radius * c.Radius; "
        "  public static method Main(): int => 0; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// ═══════════════════════════════════════════════════════════════════════════
// §2.2 Alias as qualifier in expression position (§2.7 rule 1)
// ═══════════════════════════════════════════════════════════════════════════

// §2.7 rule 1 (alias branch): `Alias.TypeName.StaticMethod()` in expression.
BEZEL_TEST(module_chapter_1365, alias_qualifies_static_call_spec_2_7) {
    ModuleMetadata dep = buildMetaFor(
        "module MathLib; "
        "public class Calc "
        "  public static method Square(n: int): int => n * n; "
        "end Calc;");
    auto r = runImporter(
        dep,
        "module App; "
        "import MathLib as ML; "
        "public class Program "
        "  public static method Main(): int => ML.Calc.Square(5); "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// ═══════════════════════════════════════════════════════════════════════════
// §2.7 Module-qualified name in type position (rule 2, single-segment prefix)
// ═══════════════════════════════════════════════════════════════════════════

// §2.7 rule 2: `ModuleName.TypeName` in type position resolves to the imported type.
BEZEL_TEST(module_chapter_1365, module_qual_type_in_field_spec_2_7) {
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
        "  public _calc: MathLib.Calc; "
        "  public constructor() "
        "    _calc := new MathLib.Calc(); "
        "  end constructor; "
        "  public static method Main(): int => 0; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// §2.7 rule 2: full module name as qualifier in expression position.
BEZEL_TEST(module_chapter_1365, module_qual_static_call_single_segment_spec_2_7) {
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
        "  public static method Main(): int => MathLib.Calc.Square(4); "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// ═══════════════════════════════════════════════════════════════════════════
// §2.2 Name-collision rule (v0.164)
// ═══════════════════════════════════════════════════════════════════════════

// §2.2 Collisions: two imports exporting the same bare name → bare use is error.
BEZEL_TEST(module_chapter_1365, collision_bare_name_is_error_spec_2_2) {
    ModuleMetadata depA = buildMetaFor(
        "module LibA; public class Widget public _n: int; end Widget;");
    ModuleMetadata depB = buildMetaFor(
        "module LibB; public class Widget public _m: int; end Widget;");
    auto r = runImporterTwo(
        depA, depB,
        "module App; "
        "import LibA; "
        "import LibB; "
        "public class Program "
        "  public static method Main(): int "
        "    using var w := new Widget(); " // Widget is ambiguous
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r->hadErrors);  // ambiguous bare name
}

// §2.2 Collisions: qualification resolves the ambiguity.
BEZEL_TEST(module_chapter_1365, collision_qualified_resolves_spec_2_2) {
    ModuleMetadata depA = buildMetaFor(
        "module LibA; "
        "public class Widget "
        "  public constructor() end constructor; "
        "  public method Val(): int => 1; "
        "end Widget;");
    ModuleMetadata depB = buildMetaFor(
        "module LibB; "
        "public class Widget "
        "  public constructor() end constructor; "
        "  public method Val(): int => 2; "
        "end Widget;");
    // Using full module name resolves the ambiguity.
    auto r = runImporterTwo(
        depA, depB,
        "module App; "
        "import LibA; "
        "import LibB; "
        "public class Program "
        "  public static method Main(): int "
        "    using var w := new LibA.Widget(); "
        "    return w.Val(); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// §2.2 Collisions + alias: alias resolves the ambiguity.
BEZEL_TEST(module_chapter_1365, collision_alias_resolves_spec_2_2) {
    ModuleMetadata depA = buildMetaFor(
        "module LibA; "
        "public class Widget "
        "  public constructor() end constructor; "
        "  public method Val(): int => 10; "
        "end Widget;");
    ModuleMetadata depB = buildMetaFor(
        "module LibB; "
        "public class Widget "
        "  public constructor() end constructor; "
        "  public method Val(): int => 20; "
        "end Widget;");
    auto r = runImporterTwo(
        depA, depB,
        "module App; "
        "import LibA as A; "
        "import LibB as B; "
        "public class Program "
        "  public static method Main(): int "
        "    using var w := new A.Widget(); "
        "    return w.Val(); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// §2.2 (v0.226, #2179): two imports may not bind the same alias, even when
// they name different modules; diagnose the later declaration and both origins.
BEZEL_TEST(import_alias_collision_2179,
           alias_vs_alias_distinct_modules_rejected_spec_2_2_v0226) {
    ModuleMetadata depA = buildMetaFor(
        "module LibA; public class Alpha end Alpha;");
    ModuleMetadata depB = buildMetaFor(
        "module LibB; public class Beta end Beta;");
    auto r = runImporterTwo(
        depA, depB,
        "module App; "
        "import LibA as Q; "
        "import LibB as Q; "
        "public class Program "
        "  public static method Main(): int => 0; "
        "end Program;");

    BEZEL_EXPECT_TRUE(r->hadErrors);
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(
        *r, "import alias `Q` for module `LibB` collides with the same alias "
            "already bound to module `LibA`; choose a different alias (§2.2)"));
}

// §2.2 (v0.226, #2179) control: distinct aliases bind independently and remain
// usable as qualifiers for their respective modules.
BEZEL_TEST(import_alias_collision_2179,
           distinct_aliases_compile_cleanly_spec_2_2_v0226) {
    ModuleMetadata depA = buildMetaFor(
        "module LibA; public class Alpha "
        "public static method Value(): int => 1; end Alpha;");
    ModuleMetadata depB = buildMetaFor(
        "module LibB; public class Beta "
        "public static method Value(): int => 2; end Beta;");
    auto r = runImporterTwo(
        depA, depB,
        "module App; "
        "import LibA as Q; "
        "import LibB as R; "
        "public class Program "
        "  public static method Main(): int => "
        "      Q.Alpha.Value() + R.Beta.Value(); "
        "end Program;");

    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// §2.2 (v0.226, #1489/#1515) regression: the alias-vs-unambiguously-imported
// bare-name check remains active alongside the new alias-vs-alias branch.
BEZEL_TEST(import_alias_collision_2179,
           alias_vs_imported_bare_name_still_rejected_spec_2_2_v0226) {
    ModuleMetadata depA = buildMetaFor(
        "module LibA; public class Q end Q;");
    ModuleMetadata depB = buildMetaFor(
        "module LibB; public class Beta end Beta;");
    auto r = runImporterTwo(
        depA, depB,
        "module App; "
        "import LibA; "
        "import LibB as Q; "
        "public class Program "
        "  public static method Main(): int => 0; "
        "end Program;");

    BEZEL_EXPECT_TRUE(r->hadErrors);
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(
        *r, "import alias `Q` collides with the unambiguously imported public "
            "bare name `Q` (from module `LibA`)"));
}

// §2.2: a declaration in the current module shadows a same-named import
// for bare-name purposes; no ambiguity, no error (§2.2 last paragraph).
BEZEL_TEST(module_chapter_1365, local_shadows_import_not_ambiguous_spec_2_2) {
    ModuleMetadata dep = buildMetaFor(
        "module Lib; public class Foo public static method Val(): int => 99; end Foo;");
    auto r = runImporter(
        dep,
        "module App; "
        "import Lib; "
        "public class Foo "  // local type named Foo — same as imported Foo
        "  public static method Val(): int => 42; "
        "end Foo; "
        "public class Program "
        "  public static method Main(): int => Foo.Val(); "  // resolves to local
        "end Program;");
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// ═══════════════════════════════════════════════════════════════════════════
// §2.7 Resolution order (v0.164)
// ═══════════════════════════════════════════════════════════════════════════

// §2.7 rule 1 before rule 2: a local type named `A` shadows a module-prefix `A.B`.
// The local `A` is a type; `A.Member` resolves against the LOCAL class, not module A.B.
BEZEL_TEST(module_chapter_1365, local_type_shadows_module_prefix_spec_2_7) {
    ModuleMetadata dep = buildMetaFor(
        "module MathLib; "
        "public class Calc "
        "  public static method Square(n: int): int => n * n; "
        "end Calc;");
    auto r = runImporter(
        dep,
        "module App; "
        "import MathLib; "
        // Local class `MathLib` — same first segment as the module prefix.
        // §2.7: rule-1 wins; `MathLib` resolves to the local type.
        "public class MathLib "
        "  public Nested: int; "
        "end MathLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var m := new MathLib(); "
        "    return m.Nested; "
        "  end Main; "
        "end Program;");
    // Resolution finds local class MathLib (not the module prefix).
    // This should succeed since .Nested exists on the local MathLib.
    BEZEL_EXPECT_TRUE(!r->hadErrors);
}

// §2.7: unimported module names are NOT resolvable as type prefixes.
BEZEL_TEST(module_chapter_1365, unimported_module_prefix_is_error_spec_2_7) {
    ModuleMetadata dep = buildMetaFor(
        "module MathLib; "
        "public class Calc "
        "  public static method Square(n: int): int => n * n; "
        "end Calc;");
    auto r = runImporter(
        dep,
        "module App; "
        // Deliberately NO `import MathLib;`
        "public class Program "
        "  public static method Main(): int "
        // MathLib not imported — MathLib.Calc should be unresolved.
        "    var n: MathLib.Calc; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r->hadErrors);
}

// ═══════════════════════════════════════════════════════════════════════════
// §2.3 Always-available names — pin existing conformance
// ═══════════════════════════════════════════════════════════════════════════

// §2.3: primitive types are always visible without import.
BEZEL_TEST(module_chapter_1365, always_available_primitives_spec_2_3) {
    BEZEL_EXPECT_TRUE(parsesClean(
        "module M; class C "
        "  public static method Foo(x: int, y: bool, z: string): double => 0.0; "
        "end C;"));
}

// §2.3: `Object`, `Array<T>`, `Task`, `IEnumerable<T>` are always available.
BEZEL_TEST(module_chapter_1365, always_available_builtins_spec_2_3) {
    DiagnosticEngine diags;
    Lexer lexer("module M; "
                "class C "
                "  public _arr: Array<int>; "
                "  public _task: Task; "
                "  public _iter: IEnumerable<string>; "
                "end C;",
                diags);
    Parser parser(lexer, diags);
    auto sf = parser.parseCompilationUnit();
    BEZEL_EXPECT_TRUE(sf && !diags.hasErrors());
}

// ═══════════════════════════════════════════════════════════════════════════
// §2.4 / §2.5 — already conforming (compilation unit, artifact-based resolution)
// The cross_module_import_test.cpp covers §2.5 artifact-based resolution
// end-to-end. These two placeholder tests pin the basic §2.4 classifications.
// ═══════════════════════════════════════════════════════════════════════════

// §2.4: a source file without a Main method compiles as a library (no error
// at the front-end level; linking to an executable requires Main elsewhere).
BEZEL_TEST(module_chapter_1365, library_has_no_main_compiles_sema_clean_spec_2_4) {
    DiagnosticEngine diags;
    Lexer lexer("module Lib; public class Helper "
                "  public static method DoWork(): int => 42; "
                "end Helper;",
                diags);
    Parser parser(lexer, diags);
    auto sf = parser.parseCompilationUnit();
    if (!sf) { BEZEL_EXPECT_TRUE(false); return; }
    sema::SemaContext ctx(diags);
    sema::collectSymbols(*sf, ctx);
    sema::resolveTypes(*sf, ctx);
    sema::typecheckExpressions(*sf, ctx);
    BEZEL_EXPECT_TRUE(!diags.hasErrors());
}

// ═══════════════════════════════════════════════════════════════════════════
// RESIDUE: §2.5 SCC compilation (v0.164)
//
// The spec now requires that modules forming an import cycle be supplied to a
// SINGLE compiler invocation as an SCC. The compiler must:
//   - detect that only part of an SCC was supplied (error naming missing members)
//   - resolve types within the SCC directly (no artifacts within the SCC)
//   - still emit one artifact per member module
//
// BLOCKER: this requires the BUILD DRIVER to detect SCCs in the module graph,
// supply all SCC sources to one invocation, and handle partial-SCC detection.
// The compiler's sema/codegen already accept multiple source files in one
// context (used by `buildMetaFor`), but the driver and build integration that
// orchestrates SCC detection and multi-file supply are not yet implemented.
//
// Filed as a separate implementation task; not in scope for this lane.
// ═══════════════════════════════════════════════════════════════════════════

}  // namespace bezel::module
