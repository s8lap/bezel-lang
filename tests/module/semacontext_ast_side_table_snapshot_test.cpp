// A SNAPSHOT of every AST-keyed side table on `sema::SemaContext`, so that
// adding a seventh one is a loud test failure rather than a silent new bug.
//
// WHY THIS EXISTS.  §2.5/§14.1 body reconstruction re-parses an imported
// generic member's body into a FRESH SemaContext and then carries the results
// back into the importer's context.  Anything the sema pass recorded in a
// table keyed on an AST node must be carried too — and every table that was
// NOT carried has produced a silent wrong value:
//
//   #2818  `exprSymbols_`   LocalSymbol dropped   -> every local read = 0
//   #2839  `lambdaParams_`  never copied          -> every lambda = 0
//   #2856  (same, static field initializer path)  -> silently 0
//   #2855  (same, destructor path)                -> SIGSEGV in the compiler
//   #2857  `patternSymbols_` carried nowhere      -> mislocated compile error
//
// Each was found one at a time, by someone tripping over it. The #2839 lane
// enumerated the tables and closed the question *for the class as it stands
// today* — but noted the gap this test fills: nothing mechanically ties "a new
// table keyed on an AST node" to "the reconstruction loops must decide about
// it", so a seventh table would silently re-acquire the whole bug class.
//
// WHAT THIS TEST DOES.  It scans `src/sema/SemaContext.hpp` for declarations of
// the form `std::unordered_map<const ast::…> name_;` and asserts the set of
// member names matches the list below.  It deliberately does NOT check that a
// table is carried — that is a judgement each table needs individually, and
// several are correctly not carried.  It only forces the judgement to be MADE.
//
// ⚠ IF THIS TEST FAILS because you added a table: do not just add the name.
// Decide, and record the decision here, whether
// `ModuleLoader::parseAndAttach{MethodBody,CtorBody,DtorBody,…}` must carry it.
// If it must, carry it in the same commit — that is the whole point.
//
// Matching is on the member NAME, which is stable under reformatting; the
// scan tolerates a declaration wrapped across lines (`exprExtBindings_` is).

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "support/repo_root.hpp"
#include "test_harness.hpp"

namespace {

// The six tables as of v0.240, each with the disposition the module loader's
// reconstruction takes. Kept beside the names deliberately: the disposition is
// the part a reader needs when this test fails.
struct Entry {
    const char* name;
    const char* disposition;
};
const Entry kAstKeyedTables[] = {
    {"resolvedTypes_",
     "CARRIED verbatim by all four paths (method/ctor/field-init; dtor #2855)"},
    {"exprTypes_",
     "CARRIED verbatim by all four paths (method/ctor/field-init; dtor #2855)"},
    {"exprSymbols_",
     "REMAPPED per symbol kind; Local carried (#2290 ctor, #2818 method, "
     "#2855 dtor), lambda Parameter carried (#2839 method+ctor, #2855 dtor; "
     "field-init carries it via #2375's verbatim else)"},
    {"patternSymbols_",
     "CARRIED verbatim by all four paths (#2857). Method + ctor + dtor are "
     "EXERCISED by xmod_generic_body_pattern_const_2857_test. The field-init "
     "path is carried for consistency but has NO KNOWN REACHABLE SHAPE: "
     "setPatternSymbol fires only for a compile-time-const LocalSymbol, and a "
     "field initializer is an expression with no locals in scope"},
    {"lambdaParams_",
     "CARRIED verbatim by all four paths: method + ctor (#2839), "
     "field-init (#2856), dtor (#2855)"},
    {"exprExtBindings_",
     "NOT carried, and proven UNREACHABLE: a body naming an extension method "
     "declines reconstruction at #783's gate before this table is consulted"},
    {"newExprPositionalBindings_",
     "NOT carried. The `new`-site half is proven UNREACHABLE: a §22.5 "
     "binding's `NewExpr` keys are `new T(args)` call SITES, which by "
     "§2.5/§2.8.3 rule 4 live in the IMPORTER's AST — every "
     "`new ImportedT(args)` is in the importer's body, and every reparse path "
     "(`parseAndAttach*` for method/ctor/dtor/field-init) re-parses DEPENDENCY "
     "bodies, not the importer's. A `new T()` inside a re-parsed body is "
     "always a §22.5/§22.10 call on a LOCAL class (whose binding would never "
     "have shipped through this table's `setNewExprPositionalBinding`) — it is "
     "published only by `checkNewExprArgs`, which runs only after the importer "
     "has resolved the type. The fresh-context reparse for an imported generic "
     "body's inner class cannot have a reachable #3196 binding. (fix #3196: "
     "SemaContext.hpp setNewExprPositionalBinding.) "
     "⚠ fix #3583 widened the KEY to `const ast::Expr*` and added a SECOND "
     "kind of key: the `this(...)` chain `CallExpr` of a §22.10 convenience "
     "constructor, published by `ExprTC::publishPrimaryCtorChainBinding` and "
     "read by `emitChainCall`'s primary arm. That key CAN occur in a "
     "re-parsed DEPENDENCY body — the argument the `NewExpr` half rests on "
     "does not cover it — so the disposition for the chain half is NOT "
     "\"unreachable\" but \"not carried, and INERT because the consumer is "
     "not reached\": an imported class that declares a constructor alongside "
     "its primary constructor never has that constructor lowered at all "
     "(#2658's degraded-construction warning; measured identical on 907ce0f "
     "and on #3583's tree — `new WithConv(7, \"direct\")` across a module "
     "boundary answers 0/empty on both). If #2658 is ever closed, this table "
     "must be re-examined: an uncarried chain binding degrades the chain-site "
     "store to the legacy all-positional pairing, not to a crash."},
    {"stmtOperatorSymbols_",
     "NOT carried, and INERT for a different reason than the WRITE being "
     "unreachable (contrast the `new`-site half above). fix #3628 (§22.15 "
     "rule 6 / §25.1 / §17.6, v0.286) publishes this table from "
     "`ExprTypecheck.cpp`'s compound-assignment and IncrementStmt overload "
     "resolution, which runs unconditionally inside `typecheckExpressions` "
     "— so a re-parsed DEPENDENCY body's own `a += b` / `t++` DOES populate "
     "`freshCtx.stmtOperatorSymbols_` when ModuleLoader typechecks it (all "
     "six `parseAndAttach{CtorBody,DtorBody,MethodBody,PropertyBody,"
     "ExtensionPropertyBody,FieldInit}` functions in ModuleLoader.cpp call "
     "`typecheckExpressions(*sf, freshCtx)`). What is unreachable is the "
     "READ: this table's sole consumer, `PureBodyWalker::checkOneStmt`'s "
     "AssignmentStmt/IncrementStmt arms (inside `checkPureBodies`), is "
     "invoked from exactly 3 call sites, all in `Driver.cpp`, always with "
     "the driver's own top-level `SemaContext` for the `SourceFile` being "
     "compiled — never `freshCtx` (0 occurrences of `checkPureBodies` in "
     "ModuleLoader.cpp). And `checkPureBodies` itself walks only "
     "`ctx.module()`'s own declared types/nested types/extensions "
     "(`forEachTypeAndNested`), never a dependency's re-parsed AST — so "
     "even carrying this table would not make it reachable; the consumer "
     "would still never visit the node it is keyed on. A `freshCtx` entry "
     "is therefore written but structurally never read: purity is verified "
     "exactly once, when the pure member's OWN declaring module is compiled "
     "as the root — the same principle the sibling narrow #3610 "
     "EXPRESSION-form check (`ctx_.exprSymbol`, read by the SAME "
     "`checkPureBodies` walk) already relies on, since it too never runs "
     "against a reparsed dependency body despite `exprSymbols_` itself "
     "being carried (the carry serves codegen's monomorphization, a "
     "different consumer that DOES operate on the reparsed AST). "
     "⚠ A PRE-EXISTING, orthogonal gap this table's reachability analysis "
     "surfaced (not introduced by #3628): even the ORIGINAL declaring "
     "compile's own `checkPureBodies` pass conservatively SKIPS a compound-"
     "assignment/increment operator resolved against an unresolved generic "
     "type parameter — `ExprTypecheck.cpp`: 'A Named RHS with NO "
     "declaration (an unresolved name or a type parameter) is left "
     "unchecked, conservatively, as before' — so a `pure` generic method "
     "whose compound-assignment/increment operator resolves to an IMPURE "
     "overload only for some concrete instantiation is caught at NEITHER "
     "the declaring compile (T unresolved) NOR the reparse (checkPureBodies "
     "never runs on freshCtx). If that conservative skip is ever tightened, "
     "or `checkPureBodies` is ever extended to run per-instantiation, this "
     "table's disposition must be re-examined together with it."},
};

// Every member name declared as `std::unordered_map<const ast::…> name_;`,
// in declaration order. Scans forward from each match to the terminating `;`
// so a declaration wrapped across lines is handled.
std::vector<std::string> scanAstKeyedTableNames(const std::string& src) {
    std::vector<std::string> out;
    const std::string needle = "std::unordered_map<const ast::";
    for (std::size_t at = src.find(needle); at != std::string::npos;
         at = src.find(needle, at + 1)) {
        // Skip accessor return types in the public section: those are preceded
        // by `const ` and followed by `&`, and declare no member.
        std::size_t semi = src.find(';', at);
        if (semi == std::string::npos) break;
        std::string decl = src.substr(at, semi - at);
        if (decl.find('&') != std::string::npos) continue;  // accessor signature
        // The member name is the last identifier before the `;`.
        std::size_t end = decl.find_last_not_of(" \t\r\n");
        if (end == std::string::npos) continue;
        std::size_t begin = end;
        while (begin > 0 &&
               (std::isalnum(static_cast<unsigned char>(decl[begin - 1])) ||
                decl[begin - 1] == '_'))
            --begin;
        std::string name = decl.substr(begin, end - begin + 1);
        if (!name.empty() && name.back() == '_') out.push_back(name);
    }
    return out;
}

} // namespace

BEZEL_TEST(semacontext_ast_side_table_snapshot,
           every_ast_keyed_table_has_a_recorded_reconstruction_disposition) {
    const std::string root = ::bezel::test::repoRoot();
    BEZEL_EXPECT_TRUE(!root.empty());
    const std::string header = root + "/src/sema/SemaContext.hpp";

    std::ifstream in(header);
    BEZEL_EXPECT_TRUE(in.good());  // header findable from __FILE__
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::vector<std::string> found = scanAstKeyedTableNames(ss.str());

    // Every declared table must be in the snapshot.
    for (const std::string& name : found) {
        bool known = false;
        for (const Entry& e : kAstKeyedTables)
            if (name == e.name) known = true;
        if (!known) {
            std::cout << "    NEW AST-keyed side table on SemaContext: " << name
                      << "\n    Decide whether ModuleLoader's body"
                         " reconstruction must carry it (see #2818/#2839),"
                         " then add it to kAstKeyedTables with that"
                         " disposition.\n";
        }
        BEZEL_EXPECT_EQ(known, true);
    }

    // And every snapshot entry must still exist — a removed table should
    // retire its entry, not leave a stale claim about the loader's behaviour.
    for (const Entry& e : kAstKeyedTables) {
        bool present = false;
        for (const std::string& name : found)
            if (name == e.name) present = true;
        if (!present)
            std::cout << "    snapshot lists a table that no longer exists: "
                      << e.name << " (" << e.disposition << ")\n";
        BEZEL_EXPECT_EQ(present, true);
    }

    BEZEL_EXPECT_EQ(found.size(),
                    sizeof(kAstKeyedTables) / sizeof(kAstKeyedTables[0]));
}
