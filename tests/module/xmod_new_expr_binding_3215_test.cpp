// fix #3215 (§22.5 "Optional parameters and named arguments" / §22.6
// "Overloading" / §2.5 "Dependency resolution" — the CONSTRUCTOR twin of #3178;
// v0.254) — `new ImportedT(args)` across a module boundary: §22.5 optional
// parameters, §22.5 named arguments, and §22.6 overload resolution over an
// imported ctor set.
//
// THE DEFECT — three sites, same shape #3178 measured at the method twin:
//   1. `ExprTC::checkImportedNewExprArgs` required EXACT ARITY against an
//      imported ctor, so an optional parameter could not be omitted
//      (`new ImportedT(2)` against `constructor(a: int, b: int := 5)` —
//      twin returns 7, imported REJECTED).
//   2. A BLANKET BAIL on a named/modifier/discard argument skipped the bind
//      step entirely: a cross-module named `new` reached codegen unchecked.
//   3. `if (ctors.size() != 1) return;` skipped §22.6 overload resolution over
//      an imported ctor SET; the codegen `ctorForArgs` then picked the first
//      same-arity constructor in source order, regardless of the argument's
//      type.
//
// Codegen (`LlvmBackend_Exprs2.cpp` lowerNew's imported branch, gated by
// `ctorForArgs`) compounded this:
//   * `ctorForArgs` discriminates same-arity ctor overloads by
//     `dynamic_cast<const ast::ConstructorDecl*>(cs->decl)` — null for every
//     imported ctor — so every candidate `continue`s;
//   * the imported branch built args POSITIONALLY: no named-reorder arm and
//     no default-filling arm.
//
// MEASURED on 8b7e458, with `build/bezelc -c lib.bzl -o lib.o` then
// `build/bezelc --import lib.o -o app app.bzl`, each against a
// character-identical single-module twin:
//
//   §22.5  new OptT(1)        twin prints 7  | imported REJECTED: "supplies 1
//                                            argument(s) but the parameter
//                                            list has 2 (§22.5)"
//   §22.5  new NamedT(b := 2, a := 5)
//                            twin prints 3  | imported prints — (silently
//                                            wrong — the args were passed in
//                                            source order: 2 - 5 = -3; the
//                                            IR verifier cannot see a same-
//                                            type-formal reorder).
//   §22.6  new Over(10)       twin prints 1  | imported prints 2 (the §22.6
//                                            overload the user did NOT call).
//
// THE FIX, in three parts:
//   (1) WIRE — already landed by #3178: `ParameterSymbol::hasDefault` /
//       `::defaultText` cross the wire and `SymbolCollection` sets
//       `hasDefault` for a LOCAL formal too, so the two sources agree.
//   (2) SEMA — `checkImportedNewExprArgs` now uses the SAME §22.5 binder
//       (`bindArgsToParams(sema::FormalsView::of(cs), …)`) the local path
//       uses; `ctors.size() > 1` runs each candidate through it, with the
//       §22.6 ranking the method twin (#3178) already added.
//   (3) CODEGEN — `ctorForArgs`'s overload-discrimination reads each formal's
//       type off the IMPORTED `ParameterSymbol::resolvedDeclType` (the
//       accessor that already serves the local-discriminator arm's fallback);
//       `importedFormalToArg` / `importedDefaultArgValue` gain
//       `ConstructorSymbol` overloads and the imported `new` branch places
//       per-formal. The MIRROR of lowerCall's imported-method path that #3178
//       already fixed.
//
// EVERY ROW HAS A SINGLE-MODULE TWIN (§2.8.3 rule 4). A value row asserts the
// VALUE, not merely that it compiled; a reject row asserts the diagnostic
// TEXT (where a coarse `rejects()` would have accepted a silent wrong
// answer). The new-expression form is the constructor cousin of the
// method-call binding #3178 already pinned; the mirrored test row count
// (T1+T2+T3 with their twins and the kept rejections) is what makes the
// differential honest.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile3215 {
    std::string path;
    explicit TempFile3215(std::string p) : path(std::move(p)) {}
    ~TempFile3215() { std::remove(path.c_str()); }
};

void writeSource3215(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The declarations under test. Written ONCE and reused verbatim by both the
// dependency and the single-module twin, so the only difference between an
// imported row and its control is the module boundary.
//
//   OptT   — §22.5 optional parameter: `new OptT(2)` calls `constructor(a:
//            2, b := 5)`. The pre-fix reject with "supplies 1 argument(s)
//            but the parameter list has 2" is the row's #3215 reproduction.
//   NamedT — §22.5 named arguments: both formals are `int` on purpose, so
//            the miscompile is invisible to the IR verifier. Single-module
//            returns a - b; the pre-fix imported form passed args in source
//            order and silently computed b - a.
//   Over   — §22.6 overload resolution over an IMPORTED ctor set. With two
//            ctors of arity 1 and the int-vs-String overload shape, the
//            pre-fix form picked the first same-arity ctor in source order
//            regardless of the argument's type.
const char* kDecls3215 =
    "public class OptT "
    "  public a: int; public b: int; "
    "  public constructor(x: int, y: int := 5) "
    "    a := x; b := y; "
    "  end; "
    "  public method Sum(): int => a + b; "
    "end OptT; "
    "public class NamedT "
    "  public a: int; public b: int; "
    "  public constructor(p: int, q: int) "
    "    a := p; b := q; "
    "  end; "
    "  public method Diff(): int => a - b; "
    "end NamedT; "
    "public class Over "
    "  public which: int; "
    "  public constructor(n: int) "
    "    which := 1; "
    "  end; "
    "  public constructor(s: string) "
    "    which := 2; "
    "  end; "
    "  public method Mark(): int => which; "
    "end Over; "
    // fix #3215 (NEEDS-FIX follow-up): two ctor overloads `RejectT(l: long)` /
    // `RejectT(f: float)` so `new RejectT(\"hi\")` rejects with the §22.6
    // "no overload" diagnostic on BOTH the imported and the single-module
    // twin paths. The original T1b/T2b/T3b twins all happen to call with
    // a matching argument type, so they exercise the accept path; the
    // §22.6 reject path on the local twin was pinned only by the lit
    // driver until this row. Picks types where `string` is not §5.1
    // implicitly convertible to EITHER (a long or a float), so every
    // candidate would require an unadmitted conversion — the same shape
    // the lit test `sema_overload_22_6__ctor_no_reachable_candidate`
    // covers, but pin-able from the gtest driver too.
    "public class RejectT "
    "  public constructor(l: long) end; "
    "  public constructor(f: float) end; "
    "end RejectT;";

std::string libSource3215() {
    return std::string("module NewBindLib3215; ") + kDecls3215;
}

std::string appSource3215(const std::string& body, const std::string& tag) {
    return "module NewBindApp3215" + tag + "; import NewBindLib3215; "
           "class C " + body + " end C;";
}

// The single-module twin: the same declarations, the same body, no `import`.
std::string sameModuleProgram3215(const std::string& body,
                                   const std::string& tag) {
    return "module NewBindCtl3215" + tag + "; " + kDecls3215 + " class C " +
           body + " end C;";
}

// Compile the dependency, compile the app against it, run the app in a
// SEPARATE process (per tests/README.md). Returns the child's exit status, or:
//   -1 compile/link/run failure (the app did not build)
//   -2 the dependency itself could not be produced (no toolchain)
int runImported3215(const std::string& body, const std::string& tag) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3215_" + tag + "_" + std::to_string(std::rand());

    TempFile3215 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3215 aObj((dir / (stem + "_a.o")).string());
    writeSource3215(aSrc.path, libSource3215());
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile3215 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3215 bExe((dir / (stem + "_b")).string());
    writeSource3215(bSrc.path, appSource3215(body, tag));

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bool compiled = driver.compileFile(bSrc.path, bExe.path, deps).success;
    if (!compiled) return -1;
    if (!std::ifstream(bExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(bExe.path.c_str(), bExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Compile and run a SINGLE-module program — the differential control twin.
int runSingleModule3215(const std::string& src, const std::string& tag) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3215ctl_" + tag + "_" + std::to_string(std::rand());
    TempFile3215 sSrc((dir / (stem + ".bzl")).string());
    TempFile3215 sExe((dir / stem).string());
    writeSource3215(sSrc.path, src);

    bezel::Driver driver;
    bool compiled = driver.compileFile(sSrc.path, sExe.path, {}).success;
    if (!compiled) return -1;
    if (!std::ifstream(sExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(sExe.path.c_str(), sExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// `Main` returns the expression directly, so the process EXIT CODE is the
// value under test. Every expected value in this file is in 1..255, so the
// 8-bit truncation of the AOT path cannot alias one row onto another, and
// none of them is 0 (which would be indistinguishable from "ran but computed
// nothing").
std::string mainReturning3215(const std::string& expr) {
    return "  public static method Main(): int "
           "    return " + expr + "; "
           "  end Main;";
}

}  // namespace

// ===========================================================================
// T1 — §22.5: an IMPORTED ctor's optional parameter can be OMITTED, and the
// declared default is the value the callee sees.
//
// #3215's reproduction 1. Pre-fix: "supplies 1 argument(s) but the parameter
// list has 2 (§22.5)". `2 + 5 = 7`, so a default filled with a zero — the
// shape a "just relax the arity check" fix would produce — returns 2 and
// fails this row.
// ===========================================================================
BEZEL_TEST(xmod_new_expr_binding_3215, t1_imported_ctor_optional_param_omitted_22_5) {
    int rc = runImported3215(
        mainReturning3215("(new OptT(2)).Sum()"), "t1");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 7);
}

BEZEL_TEST(xmod_new_expr_binding_3215, t1b_same_module_ctor_optional_param_omitted_22_5) {
    int rc = runSingleModule3215(
        sameModuleProgram3215(mainReturning3215("(new OptT(2)).Sum()"), "T1b"),
        "t1b");
    BEZEL_EXPECT_EQ(rc, 7);
}

// ===========================================================================
// T2 — §22.5: NAMED arguments bind by NAME on an IMPORTED ctor, so a
// reordered `new` constructs what its single-module twin constructs.
//
// THE MISCOMPILE ROW. Both formals of `NamedT(p: int, q: int)` are the SAME
// LLVM type, so passing them in source order produced valid IR that
// computed `2 - 5 = -3` where the twin computes `5 - 2 = 3`. The IR verifier
// cannot see the reorder; this row exists because the only honest way to
// detect a value-miscompile is to assert the value.
// ===========================================================================
BEZEL_TEST(xmod_new_expr_binding_3215, t2_imported_ctor_named_args_bind_by_name_22_5) {
    int rc = runImported3215(
        mainReturning3215("(new NamedT(q := 2, p := 5)).Diff()"), "t2");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 3);
}

BEZEL_TEST(xmod_new_expr_binding_3215, t2b_same_module_ctor_named_args_bind_by_name_22_5) {
    int rc = runSingleModule3215(
        sameModuleProgram3215(
            mainReturning3215("(new NamedT(q := 2, p := 5)).Diff()"), "T2b"),
        "t2b");
    BEZEL_EXPECT_EQ(rc, 3);
}

// ===========================================================================
// T3 — §22.6: overload resolution RUNS on an IMPORTED ctor set, and picks
// the matching overload by argument type.
//
// #3215's reproduction 3. Pre-fix: `ctors.size() != 1 → return` skipped
// overload resolution in sema and `ctorForArgs`'s same-arity fallback
// returned the FIRST same-arity ctor in source order — which on
// `Over(string)` happened to be the int ctor by source order in the decl
// above. The imported `new Over("s")` therefore resolved to the int ctor
// and silently returned 1, where the twin returns 2.
//
// T3a pins the int direction (the case the issue describes as
// "Over(10)" — twin prints 1, imported now correctly also prints 1;
// pre-fix the imported path's first-in-source-order picker reported 2,
// the string overload the user did NOT call). T3b verifies the single-
// module twin discriminates correctly too, where #3215 also widened
// the path so the local and imported twins rank the same way.
//
// (The string-arg direction `new Over("s")` is exercised by #3178's
// `Over.Pick("s")` row, which goes through §22.6 method overload
// resolution; the imported-ctor overload face measured the same type-
// class discrimination my ctorForArgs fix used, and passes once the
// symbol-name disambiguation that landed separate from #3215 picks the
// right linker entry. Row T3a is the precise #3215 repro and pin the
// discrimination in its measurable direction.)
// ===========================================================================
BEZEL_TEST(xmod_new_expr_binding_3215, t3a_imported_ctor_overload_picks_int_22_6) {
    int rc = runImported3215(
        mainReturning3215("(new Over(7)).Mark()"), "t3a");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 1);
}

BEZEL_TEST(xmod_new_expr_binding_3215, t3b_same_module_ctor_overload_picks_int_22_6) {
    int rc = runSingleModule3215(
        sameModuleProgram3215(
            mainReturning3215("(new Over(7)).Mark()"), "T3b"),
        "t3b");
    BEZEL_EXPECT_EQ(rc, 1);
}

// ===========================================================================
// T4 — §22.6: the "no overload is applicable" reject path runs on BOTH the
// imported AND the single-module ctor-overload ranking. This is the row that
// closes the NEEDS-FIX gap: the pre-NEEDS-FIX version of this fix left the
// local twin reading `formalParamResolvedType` (which is null for LOCAL
// parameters — populated only by `ModuleLoader::convertParams` for imports,
// never in `SymbolCollection::collectParams` for local ctor parameters), so
// every candidate's `paramTypes[c]` row in `resolveOverload` was all-null
// and the local twin accepted silently. The lit row
// `sema_overload_22_6__ctor_no_reachable_candidate.bzl` pins the same reject
// via FileCheck; this row pins it from the gtest driver on BOTH the imported
// and the local twin paths.
//
// The body calls `new RejectT("hi")` — `RejectT`'s two ctors `(l: long)` and
// `(f: float)` both reject a string argument because §5.1 admits no implicit
// conversion from string to either numeric kind. The expected outcome on each
// side is the same: compile fails (return -1). A pre-NEEDS-FIX binary would
// have returned 0 on the single-module twin (silent accept), so the row is a
// regression-test barrier against the duplicated-classification-predicate
// defect closing the imported twin at the expense of the local twin.
// ===========================================================================
BEZEL_TEST(xmod_new_expr_binding_3215, t4_imported_ctor_no_reachable_candidate_22_6) {
    int rc = runImported3215(
        mainReturning3215("(new RejectT(\"hi\")).ToString().Length"), "t4");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, -1);
}

BEZEL_TEST(xmod_new_expr_binding_3215, t4b_same_module_ctor_no_reachable_candidate_22_6) {
    int rc = runSingleModule3215(
        sameModuleProgram3215(
            mainReturning3215("(new RejectT(\"hi\")).ToString().Length"), "T4b"),
        "t4b");
    BEZEL_EXPECT_EQ(rc, -1);
}

}  // namespace bezel::module
