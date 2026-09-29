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
     "store to the legacy all-positional pairing, not to a crash. "
     "⚠ fix #4484 added a THIRD kind of key of the same shape: the "
     "`base(...)` chain `CallExpr` of an explicit derived constructor whose "
     "call binds the BASE class's primary parameter list, published by "
     "`ExprTC::publishBaseChainPrimaryBinding` and read by codegen's "
     "`Emitter::prePopulateBasePositionalFields` (case (b)). NOT carried — "
     "but do NOT read that as the `this(...)` half's disposition restated. "
     "The two halves are uncarried for DIFFERENT reasons and with different "
     "consequences, and the distinction is the whole value of this entry. "
     "Where a `base(...)` chain sits decides which: "
     "(1) DERIVED CLASS IN THE IMPORTER, base imported — by far the common "
     "shape. The `CallExpr` is in the importer's own AST, so no reparse is "
     "involved, the binding is published and read normally, and #4484 FIXES "
     "it. Measured, `public class B(n: int, s: string)` in `LibY` and "
     "`public class D : B` with `base(s := \"x\", n := 1)` in the importer: "
     "pre-fix printed `4434352` then SIGSEGV (exit 139); post-fix `1` / `x`, "
     "exit 0. "
     "(2) THE CHAIN INSIDE A RE-PARSED DEPENDENCY BODY — a generic "
     "`public class G<T> : B` whose named `base(...)` lives in the "
     "dependency and is instantiated in the importer, so `ModuleLoader` "
     "re-parses it into `freshCtx`. This is the shape this table is about, "
     "and it is NOT fixed: measured BIT-IDENTICAL before and after #4484 in "
     "every variant below. "
     "⚠ THIS SHAPE CRASHES OR NOT DEPENDING ON THE CONSUMER, AND THE CRASH "
     "IS NOT #4484's. Both outcomes are real and a reader who reconstructs "
     "only one will misjudge the severity, so both are recorded with what "
     "actually decides between them. In shape (2) the base's positional "
     "slots are calloc-zero in EVERY variant — the pre-population does not "
     "run at all, so the textual mispairing never happens — which leaves "
     "`n` reading 0 and `s` a NULL string pointer. What varies is only how "
     "the program then CONSUMES that null: "
     "`Console.WriteLine(s)` tolerates it and prints an empty line (exit 0, "
     "`0` then blank), while `Console.WriteLine($\"n={n} s={s}\")` "
     "dereferences it and SIGSEGVs (exit 139). Measured as a 2x2 over "
     "{generic field present, absent} x {interpolated Show, two-WriteLine "
     "Show}: the `Show` body decides all four cells and the FIELD MAKES NO "
     "DIFFERENCE — an earlier reading of this that attributed the split to "
     "the generic field changing the layout, and so to where a mispaired "
     "store lands, is wrong; there is no mispaired store here to land "
     "anywhere. "
     "THE DIAGNOSTIC THAT TELLS THE TWO CAUSES APART is what `n` prints: "
     "shape (1)'s pre-fix crash prints GARBAGE for `n` (`4434352`, the "
     "string pointer's low half — the mispairing really happened), whereas "
     "shape (2) prints `0` or dies before printing (calloc-zero — it never "
     "did). A cross-module SIGSEGV is therefore NOT on its own evidence "
     "that #4484's mispairing is reachable across the seam. "
     "This is also why shape (2) is not inert in the `this(...)` half's "
     "sense: that half is inert because its CONSUMER is never reached, "
     "whereas here the consumer IS reached and merely finds no binding; what "
     "keeps the mispairing from biting is the separate pre-existing "
     "calloc-zero gap, not anything about #4484. "
     "⚠ \"Degrades rather than crashes\" is a statement about the COMPILER, "
     "not the program: the compiler falls back to the pre-#4484 textual "
     "pairing instead of failing, and on shape (1) that fallback is exactly "
     "the SIGSEGV #4484 exists to remove. If the calloc-zero gap in shape "
     "(2) is ever closed WITHOUT carrying this table, that fallback becomes "
     "reachable there too and the mispairing's OWN crash arrives on top of "
     "the null-consumer one already visible today — so closing that gap and "
     "carrying this binding are one change, not two. Reproducers: "
     "/var/tmp/bezel-scratch/c15-4484/grid/{withfield,nofield}_"
     "{interp,writeln}/ (shape 2, the full 2x2), "
     "/var/tmp/bezel-scratch/adjudicate-4484/{withfield,nofield}/ (the two "
     "probes the 2x2 was derived from) and "
     "/var/tmp/bezel-scratch/c15-4484/xmod/{lib2,app2}.bzl (shape 1)."},
    {"inheritanceClauseBindings_",
     "NOT carried, and UNREACHABLE BY KEY TYPE. fix #4491 (§22.5 / §22.8 step 0 / "
     "§22.10, v0.306) — the §22.10 INHERITANCE-CLAUSE half of the §22.5 "
     "binding family whose other three keys live on "
     "`newExprPositionalBindings_` above (the `new` site #3196, the "
     "`this(...)` chain #3583, the `base(...)` chain #4484). Published by "
     "`ExprTC::visitTopDecl`'s primary-base-ctor-args walk, read by codegen's "
     "`Emitter::prePopulateBasePositionalFields` (case (a)). "
     "It is a table of its OWN for a structural reason, not a stylistic one: "
     "the clause's arguments are a bare `std::vector<ast::CallArgument>` on "
     "the `ast::TypeDecl` with NO `ast::Expr` node to key on, and the only "
     "Expr-shaped pointer available — a clause argument's own value — "
     "COLLIDES with `checkPositionalNewArgsFromSymbols`'s key whenever that "
     "argument is itself a `new` expression "
     "(`class D(v: int) : B(s := new Q(b := 1, a := 2), n := v)`). "
     "THE KEY IS THE ARGUMENT VECTOR'S ADDRESS, i.e. exactly what "
     "`sema::primaryBaseCtorArgsOf(t)` returns, so the local/imported split "
     "is pointer identity rather than a `dynamic_cast<const "
     "ast::TypeDecl*>(t->decl)` seam read (#3152) plus a hand-written guard. "
     "It is why this table needed the SECOND needle in the scanner below. "
     "WHY THE DISPOSITION IS STRONGER THAN THE `base(...)` HALF'S ABOVE, and "
     "is not that entry restated: the vector this keys on lives on the "
     "`ast::TypeDecl` of the class DECLARING the clause, i.e. in a class "
     "HEADER. ModuleLoader's `parseAndAttach*` paths re-parse member BODIES "
     "(method, constructor, destructor, property, extension property, field "
     "initializer) into `freshCtx`; none re-parses a class header, so no "
     "`ast::TypeDecl` carrying a `primaryBaseCtorArgs` list is ever created "
     "in a fresh context and no entry can be written there to be dropped. "
     "CROSS-MODULE, MEASURED ON BOTH SIDES rather than argued: "
     "(1) NON-GENERIC — `public class B(n: int, s: string)` and "
     "`public class D(v: int) : B(s := \"clause\", n := v)` both in a library, "
     "`new D(9)` in the importer. #4491 FIXES it: pre-fix the program printed "
     "`4439440` — the string pointer's low half read as `n` — then SEGVed "
     "(exit 139); post-fix `9` / `clause`, exit 0. The clause is in the "
     "declaring module's own AST, so the binding is published and read in "
     "that module's compile and no reparse is involved. "
     "(2) GENERIC (`public class G<T>(v: int) : B(...)` in the library, "
     "`new G<int>(9)` in the importer) — NOT fixed, and NOT this defect. "
     "Post-fix it prints `0` and a blank line at exit 0: the base slots are "
     "calloc-zero because the pre-population does not run there AT ALL, so "
     "there is no mispaired store to correct. The tell is #4484's own: "
     "garbage for `n` means the mispairing happened, `0` means it did not. "
     "The generic-ness is not what decides it — the SAME generic clause in "
     "a SINGLE module prints `9` / `clause` correctly — the module seam is. "
     "If that pre-existing calloc-zero gap is ever closed for an imported "
     "class, this binding must be carried with it (an imported class's clause "
     "arguments live on `TypeSymbol::importedPrimaryBaseCtorArgs`, #3911, a "
     "vector with no `ast::TypeDecl` at all), or the textual mispairing "
     "becomes reachable there. Reproducers: "
     "/var/tmp/bezel-scratch/c15-4491/xmod/{lib,app}.bzl (case 1) and "
     "/var/tmp/bezel-scratch/c15-4491/xmod/{glib,gapp}.bzl (case 2)."},
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
    {"indexerSubstitutedParamTypes_",
     "NOT carried, and INERT for the same reason as `stmtOperatorSymbols_` "
     "above: a fresh-context reparse populates the table (fix #4084's "
     "write site is `ExprTC::checkIndexArgNullability`'s "
     "`declaredIndexParam` lambda, called from `typecheckExpressions`, "
     "which every ModuleLoader `parseAndAttach*` body invokes against "
     "`freshCtx` — §28.3/§23.4/§14.1, v0.294) but the sole consumer, "
     "`Ownership::recordCallArgDirectLambdaTargetsFromIndexer` "
     "(Ownership_Checks.cpp, fix #4075 widening for #4084's generic-"
     "instantiation case), is reached only from `Ownership::checkOwnership` "
     "(Ownership.cpp), which runs against the driver's own top-level "
     "`SemaContext` for the SourceFile being compiled (0 occurrences of "
     "`checkOwnership` in ModuleLoader.cpp — the same topology as "
     "`stmtOperatorSymbols_`'s `checkPureBodies`). A re-parsed dependency "
     "body's `freshCtx` entry is therefore structurally never read: "
     "disposal-tracked lambda targets on a generic-indexer call are "
     "verified exactly once, when the DEPENDENCY's own declaring module "
     "is compiled as the root. Pre-#4084 the read used a per-call "
     "raw-resolve fallback (the `substituteMemberType(p, ix.target.get(), "
     "ps->parent)` chain #4084 calls at the same site in the typecheck "
     "path) that closed #4075's defect for non-generic indexers; #4084's "
     "side-table just lifts the generic case onto the same machinery "
     "without changing the topology."},
    {"inheritanceClauseCtorBindings_",
     "NOT carried, and UNREACHABLE BY KEY TYPE — the same disposition, for "
     "the same structural reason, as `inheritanceClauseBindings_` above, and "
     "recorded separately because it is a DIFFERENT question about the same "
     "clause. fix #4520 (§22.5 / §22.8 step 1 / §22.10, v0.306) is the THIRD "
     "and last §22.5 site of that family: a §22.10 inheritance clause that "
     "binds an EXPLICIT base constructor rather than the base's PRIMARY "
     "parameter list. Published by `ExprTC::visitTopDecl`'s "
     "primary-base-ctor-args walk, read by codegen's "
     "`Emitter::emitImplicitBase` (the `baseClauseExplicitCtor` arm, "
     "LlvmBackend_Setup.cpp) — a different lowering site from the sibling "
     "table's `prePopulateBasePositionalFields` case (a), against a different "
     "formal list. It is a table of its OWN rather than a second entry on "
     "that one precisely BECAUSE the key is the same: both bindings identify "
     "the same clause, so one map could hold only one of them, and serving a "
     "PRIMARY-parameter binding to a call that binds a convenience "
     "constructor's list is exactly the mispairing #3562 removed. "
     "ITS VALUE CARRIES THE RESOLVED `ConstructorSymbol*`, which is not "
     "decoration: sema resolves the callee by ARITY and codegen by "
     "`ctorForArgs` (which also discriminates an overload set by argument "
     "type), so the consumer checks POINTER IDENTITY and falls back to the "
     "pre-#4520 positional pairing on any disagreement — a lost fix, never a "
     "wrong-slot store. The symbol is owned by the `SemaContext` whose "
     "`symbols_` arena produced it, i.e. the same context the binding is "
     "published in, so the pointer cannot outlive its map. "
     "WHY THE LOADER NEED NOT CARRY IT: the key is the address of the "
     "`primaryBaseCtorArgs` vector on the `ast::TypeDecl` of the class "
     "DECLARING the clause — a class HEADER. ModuleLoader's `parseAndAttach*` "
     "paths re-parse member BODIES only; none re-parses a class header, so no "
     "`ast::TypeDecl` carrying a `primaryBaseCtorArgs` list is ever created in "
     "a `freshCtx` and no entry can be written there to be dropped. An "
     "IMPORTED derived class reaches codegen through the replayed "
     "`TypeSymbol::importedPrimaryBaseCtorArgs` vector (#3911) — a different "
     "address — which finds no entry and keeps the textual-order pairing, so "
     "the local/imported split is pointer identity with no seam read. "
     "CROSS-MODULE, MEASURED ON BOTH SIDES rather than argued, and the "
     "answer is the OPPOSITE of the sibling table's: the common cross-module "
     "shape IS fixed here, because the class DECLARING the clause is the one "
     "whose compile publishes and reads the binding. With "
     "`public class B` (explicit `constructor(a: int, b: string)`) and "
     "`public class D(v: int) : B(b := \"e\", a := v)` both in a library and "
     "`new D(4)` in the importer: pre-fix the program printed `0` and a "
     "blank line (the base constructor never ran), post-fix `4` / `e`, exit "
     "0 both times. What is NOT carried is the reverse direction — a clause "
     "on an IMPORTED class, reached through the replayed vector — and that "
     "is the pointer-identity miss described above, not a dropped entry. "
     "MEASURED single-module (6c05b4cf → fix): the same clause printed `0` "
     "and a blank line pre-fix and `4` / `e` post-fix; over a base with BOTH "
     "a primary constructor and an arity-matching convenience constructor it "
     "printed `4431248` and then SIGSEGVed (exit 139) pre-fix — the string "
     "pointer's low half stored into the primary's `int` slot by "
     "`prePopulateBasePositionalFields` case (a), which took over precisely "
     "because this arm reported no callable constructor — and `4` / `e`, "
     "exit 0, post-fix. Reproducers: "
     "/var/tmp/bezel-scratch/c16-basedefer/repro/p_clause_explicit_named.bzl, "
     "probe/x_conv.bzl and probe/xm2/{lib,app}.bzl."},
    {"resolvedNewCtors_",
     "NOT carried; a miss is SAFE BY CONSTRUCTION, not argued safe. fix #4853 "
     "(§22.6 / §22.7, v0.307): the constructor sema's §22.6 resolution "
     "selected for an overloaded LOCAL `new T(...)` "
     "(`ExprTC::checkNewExprArgs`), keyed by the address of `NewExpr::args` "
     "— the one handle codegen's `Emitter::ctorForArgs(cls, args)` has on its "
     "call site, hence the second needle's key shape, as for "
     "`inheritanceClauseBindings_`. A `new` inside a re-parsed dependency "
     "body may publish into `freshCtx`'s copy, which codegen does not read; "
     "the consumer then finds no entry and runs its pre-#4853 search "
     "(`ctorForArgs`'s proxy tiers), i.e. exactly today's behaviour for that "
     "site. The consumer also adopts an entry only when the symbol is a "
     "non-static constructor MEMBER of the class being resolved; that "
     "guards class identity only. The key's soundness rests on every "
     "`NewExpr::args` vector outliving the compile (AST nodes are never "
     "freed mid-compile, so an address is never reused for another site's "
     "arguments). The value is owned by the "
     "`SemaContext` whose arena produced it — the one the entry lives in."},
};

// Every member name declared as an AST-keyed `std::unordered_map`, in
// declaration order. Scans forward from each match to the terminating `;` so a
// declaration wrapped across lines is handled.
//
// ⚠ TWO NEEDLES since fix #4491, and the second one is the point. A table
// keyed on an AST NODE spells `std::unordered_map<const ast::…>`. But not
// every AST-owned fact HAS a node to key on: `ast::CallArgument` is not an
// `ast::Node` (it carries no span), so a §22.10 inheritance clause's argument
// list is keyed by the ADDRESS of the `std::vector<ast::CallArgument>` living
// on its `ast::TypeDecl`, spelling `std::unordered_map<const
// std::vector<ast::…>`. That table is every bit as AST-keyed, and every bit as
// subject to the reconstruction judgement this test exists to force — and the
// single original needle would have let it in unnoticed, which is precisely
// the "seventh table" failure mode described at the top of this file. The two
// needles cannot double-count: after `const ` the second spells `std::`, so
// neither string is a substring of the other.
std::vector<std::string> scanAstKeyedTableNames(const std::string& src) {
    std::vector<std::string> out;
    const std::string needles[] = {"std::unordered_map<const ast::",
                                   "std::unordered_map<const std::vector<ast::"};
    for (const std::string& needle : needles)
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
