// fix #3178 (§22.5 "Optional parameters and named arguments" / §22.6
// "Overloading" / §5.1 / §2.5 "Dependency resolution"; v0.254) — call binding
// to an IMPORTED method: §22.6 overload ranking, §22.5 optional parameters, and
// §22.5 named arguments across a module boundary.
//
// Family M5 of umbrella #3152; see
// docs/proposals/papers/member-symbol-audit-2026-08-13.md.
//
// WHAT THE DEFECT WAS. `checkImportedCallArity` was the ENTIRE call-checking
// path for an imported callee, and `resolveMethodOverload`'s candidate
// collection required `dynamic_cast<const ast::MethodDecl*>(cand->decl)`.
// `ModuleLoader` sets `MemberSymbol::decl` only where it re-parsed body text for
// a GENERIC type, so for a non-generic imported class that cast is null for
// EVERY candidate: `cands` was provably always empty and §22.6 ranking did not
// merely pick badly across the boundary — it never ran. What remained was an
// arity-equality test guarded by a blanket bail on the first named / modifier /
// discard argument.
//
// MEASURED ON `main` @ ec6a02f, with `build/bezelc -c lib.bzl -o lib.o` then
// `build/bezelc --import lib.o -o app app.bzl`, each against a
// character-identical single-module twin:
//
//   §22.6  Over.Pick("s")            twin prints 2  | imported REJECTED:
//                                    "the call to `Pick`: argument 1 has a type
//                                    that is not implicitly convertible to the
//                                    target type (§5.1)" — a diagnostic
//                                    describing a call the user did not write,
//                                    because `arityFallback` returned the
//                                    first-in-source `Pick(int)`.
//   §22.5  Opt.WithDefault(1)        twin prints 8  | imported REJECTED:
//                                    "supplies 1 argument(s) but the parameter
//                                    list has 2 (§22.5)". `ParameterSymbol` had
//                                    no `hasDefault`, so the check demanded
//                                    exact arity.
//   §22.5  Named.Sub(b := 3, a := 10) twin prints 7 | imported prints **-7**.
//   §22.5  Named.One(nosuch := 1)    twin REJECTED  | imported prints **101**.
//   §22.5  Named.Two(b := "xyz", a := 2)  twin prints 5 | imported: malformed
//                                    IR ("Call parameter type does not match
//                                    function signature").
//
// The last three are the named-argument half, and the issue UNDERSTATED it. It
// says named arguments are "unchecked entirely — a false accept". They are that
// (`One(nosuch := 1)` bound positionally and ran), but the codegen side is
// worse: both imported-callee argument loops in LlvmBackend_Calls2.cpp walked
// `call.args` in SOURCE order with a running parameter index, so a named call's
// arguments were passed in the order they were WRITTEN. Where the formals have
// different LLVM types the IR verifier catches it; where they coincide — `Sub(a:
// int, b: int)` — nothing does, and the program silently computes a different
// answer than its single-module twin. That is a MISCOMPILE, not a missing check,
// and it is what row T3 pins.
//
// ⚠ ONE CLAIM IN #3178 IS REFUTED HERE. The issue reports the optional-parameter
// diagnostic's span as `0:0` and calls that "a second, independent defect" of
// this path. It is a real defect and it is NOT cross-module: the
// single-module twin's §22.5 rejection prints `0:0` too, because
// `ast::CallExpr::span` is never stamped by the parser (Parser_Exprs.cpp — the
// node is built from `head` + `parseArgList()` with no span assignment), so
// EVERY diagnostic reported at `ce.span.begin` has no location, local and
// imported alike. Filed separately as #3214; this fix deliberately keeps
// reporting at `ce.span.begin` so the cross-module text and location stay
// byte-identical to the local path's (the property #3141 pins and #3173 exists
// to keep).
//
// THE FIX, in three parts:
//   (1) WIRE. `MetaParam::hasDefault` / `::defaultText` were already exported by
//       ModuleMetadataBuilder and decoded by ModuleMetadata.cpp; only
//       `ModuleLoader::convertParams` dropped them. They now land on
//       `ParameterSymbol::hasDefault` / `::defaultText`. No format change.
//   (2) SEMA. `bindArgsToParams` — the ONE §22.5 binder — now takes a
//       `sema::FormalsView` (the shape #3173 specifies), so the same rule and
//       the same five diagnostic strings serve a local `ast::Param` list and an
//       imported `ParameterSymbol` list. `resolveMethodOverload`'s candidate
//       collection, its `ref`/`out` applicability filter and its §22.6 ranking
//       rows all read the formals through `sema::formalParam*` instead of a
//       `MethodDecl` cast; `checkImportedCallArity` binds first and then checks
//       the BOUND positions.
//   (3) CODEGEN. Both imported-callee argument loops evaluate in SOURCE order
//       (§22.5 "supplied argument expressions are evaluated in call-site
//       textual order") but PLACE by formal, and fill an omitted optional formal
//       from its exported default text through the existing
//       `foldImportedConstText` — the same materializer every other imported
//       textual constant already uses.
//
// KNOWN LIMIT, made LOUD rather than silent (row T8). §22.5 permits a
// NON-CONSTANT default (`DateTime.Now`, a factory call). `renderConstExpr`
// exports only literals, so such a default crosses as `hasDefault = true` with
// an empty `defaultText`. Admitting the omission on that basis would have turned
// a loud §22.5 reject into a silent codegen bail, so sema and codegen share ONE
// predicate — `sema::importedDefaultTextIsMaterializable` — and the call is
// rejected naming the limitation. Carrying the remaining forms is #3213.
//
// EVERY ROW HAS A SINGLE-MODULE TWIN (§2.8.3 rule 4). A value row asserts the
// VALUE, not merely that it compiled; a reject row asserts the diagnostic TEXT,
// because three of the five measured pre-fix outcomes were "compiles and runs"
// or "compiles and miscompiles", which a coarse `rejects()` would have accepted.

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
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile3178 {
    std::string path;
    explicit TempFile3178(std::string p) : path(std::move(p)) {}
    ~TempFile3178() { std::remove(path.c_str()); }
};

void writeSource3178(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The declarations under test. Written ONCE and reused verbatim by both the
// dependency and the single-module twin, so the only difference between an
// imported row and its control is the module boundary.
//
//   Over     — the §22.6 overload set from the issue's reproduction 1.
//   Opt      — §22.5 optional parameters, one per default LITERAL KIND that
//              `renderConstExpr` can export (int, string, bool).
//   Named    — §22.5 named arguments. `Sub`'s two formals are BOTH `int` on
//              purpose: that is the shape whose mis-ordering the IR verifier
//              cannot see, and it is the one that silently printed -7.
//   Inst     — the instance-method forms, for the `.` and `?.` call paths.
//   OverDef  — §22.6's defaulted-arity tie-break (v0.181, #1561). `Two(9)` must
//              keep choosing the NON-defaulted overload once ranking starts
//              running across the boundary for the first time.
//   NonConst — a §22.5 NON-CONSTANT default; the known limit above.
const char* kDecls3178 =
    "public class Over "
    "  public static method Pick(x: int): int => 1; "
    "  public static method Pick(x: string): int => 2; "
    "end Over; "
    "public class Opt "
    "  public static method WithDefault(a: int, b: int := 7): int => a + b; "
    "  public static method Greet(n: int, s: string := \"abc\"): int "
    "    => n + s.Length; "
    "  public static method Flag(n: int, f: bool := true): int "
    "    if f then return n + 1; end if; "
    "    return n + 2; "
    "  end Flag; "
    "end Opt; "
    "public class Named "
    "  public static method Sub(a: int, b: int): int => a - b; "
    "  public static method One(x: int): int => x + 100; "
    "  public static method Two(a: int, b: string): int => a + b.Length; "
    "end Named; "
    "public class Inst "
    "  public v: int; "
    "  public constructor(x: int) "
    "    v := x; "
    "  end; "
    "  public method Add(a: int, b: int := 40): int => v + a - b; "
    "end Inst; "
    "public class OverDef "
    "  public static method Two(a: int): int => 1; "
    "  public static method Two(a: int, b: int := 5): int => 2; "
    "end OverDef; "
    "public class NonConst "
    "  public static method Stamp(a: int, b: int := Named.One(1)): int "
    "    => a + b; "
    "end NonConst;";

std::string libSource3178() {
    return std::string("module CallBindLib3178; ") + kDecls3178;
}

std::string appSource3178(const std::string& body, const std::string& tag) {
    return "module CallBindApp3178" + tag + "; import CallBindLib3178; "
           "class C " + body + " end C;";
}

// The single-module twin: the same declarations, the same body, no `import`.
std::string sameModuleProgram3178(const std::string& body,
                                  const std::string& tag) {
    return "module CallBindCtl3178" + tag + "; " + kDecls3178 + " class C " +
           body + " end C;";
}

// Compile the dependency, compile the app against it, run the app in a SEPARATE
// process (per tests/README.md). Returns the child's exit status, or:
//   -1 compile/link/run failure (the app did not build)
//   -2 the dependency itself could not be produced (no toolchain)
// `capturedStderr`, when non-null, receives the app compile's stderr — the only
// way to assert on diagnostic TEXT.
int runImported3178(const std::string& body, const std::string& tag,
                    std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3178_" + tag + "_" + unique_stem("");

    TempFile3178 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3178 aObj((dir / (stem + "_a.o")).string());
    writeSource3178(aSrc.path, libSource3178());
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile3178 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3178 bExe((dir / (stem + "_b")).string());
    TempFile3178 bErr((dir / (stem + "_b.err")).string());
    writeSource3178(bSrc.path, appSource3178(body, tag));

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(bErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        compiled = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(bErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps).success;
    }
    if (!compiled) return -1;
    if (!std::ifstream(bExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(bExe.path.c_str(), bExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);  // 1011 = SIGSEGV
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Compile and run a SINGLE-module program — the differential control twin.
// Same return convention as runImported3178 minus the -2 case.
int runSingleModule3178(const std::string& src, const std::string& tag,
                        std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3178ctl_" + tag + "_" + unique_stem("");
    TempFile3178 sSrc((dir / (stem + ".bzl")).string());
    TempFile3178 sExe((dir / stem).string());
    TempFile3178 sErr((dir / (stem + ".err")).string());
    writeSource3178(sSrc.path, src);

    bezel::Driver driver;
    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(sErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        compiled = bezel::test::compileFileForTest(driver, sSrc.path, sExe.path, {}).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(sErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled = bezel::test::compileFileForTest(driver, sSrc.path, sExe.path, {}).success;
    }
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
// 8-bit truncation of the AOT path cannot alias one row onto another, and none
// of them is 0 (which would be indistinguishable from "ran but computed
// nothing").
std::string mainReturning3178(const std::string& expr) {
    return "  public static method Main(): int "
           "    return " + expr + "; "
           "  end Main;";
}

bool mentions3178(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// Assert that the imported and single-module compilations reject with the SAME
// §22.5 message text. The location prefix is deliberately not compared — see
// the #3214 note in this file's header.
void expectSameRejection3178(const std::string& body, const std::string& tag,
                             const char* messageText) {
    std::string impErr;
    int impRc = runImported3178(body, tag, &impErr);
    if (impRc == -2) return;  // no toolchain for the dependency
    std::string ctlErr;
    int ctlRc = runSingleModule3178(sameModuleProgram3178(body, tag),
                                    tag + "c", &ctlErr);
    BEZEL_EXPECT_EQ(impRc, -1);  // the imported form must NOT build
    BEZEL_EXPECT_EQ(ctlRc, -1);  // …and neither must its twin
    BEZEL_EXPECT_TRUE(mentions3178(impErr, messageText));
    BEZEL_EXPECT_TRUE(mentions3178(ctlErr, messageText));
}

}  // namespace

// ===========================================================================
// T1 — §22.6: overload ranking RUNS on an imported receiver, and picks the
// `string` overload for a `string` argument.
//
// This is #3178's reproduction 1. Pre-fix the imported form was REJECTED with a
// §5.1 conversion error against `Pick(int)` — the overload the user did not
// call — because `cands` was empty and `arityFallback` returned the first
// same-name symbol in source order. The row asserts the VALUE (2), not that it
// compiles: "it compiles" would also pass if the wrong overload were selected.
// ===========================================================================
BEZEL_TEST(xmod_call_binding_3178, t1_imported_overload_ranking_picks_string_22_6) {
    int rc = runImported3178(mainReturning3178("Over.Pick(\"s\")"), "t1");
    if (rc == -2) return;  // no toolchain for the dependency
    BEZEL_EXPECT_EQ(rc, 2);
}

BEZEL_TEST(xmod_call_binding_3178, t1b_same_module_overload_ranking_22_6) {
    int rc = runSingleModule3178(
        sameModuleProgram3178(mainReturning3178("Over.Pick(\"s\")"), "T1b"),
        "t1b");
    BEZEL_EXPECT_EQ(rc, 2);
}

// The `int` arm of the same overload set. Without it, a fix that simply
// reversed the source-order choice would pass T1 while breaking every
// `Pick(int)` call — the row exists to pin that ranking, not order, decides.
BEZEL_TEST(xmod_call_binding_3178, t1c_imported_overload_ranking_picks_int_22_6) {
    int rc = runImported3178(mainReturning3178("Over.Pick(4)"), "t1c");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 1);
}

// ===========================================================================
// T2 — §22.5: an imported method's optional parameter can be OMITTED, and the
// declared default is the value the callee sees.
//
// #3178's reproduction 2. Pre-fix: "call to `WithDefault` supplies 1
// argument(s) but the parameter list has 2 (§22.5)". 8 = 1 + 7, so a default
// filled with a zero — the shape a "just relax the arity check" fix would
// produce — reads 1 and fails this row.
// ===========================================================================
BEZEL_TEST(xmod_call_binding_3178, t2_imported_optional_param_omitted_22_5) {
    int rc = runImported3178(mainReturning3178("Opt.WithDefault(1)"), "t2");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 8);
}

BEZEL_TEST(xmod_call_binding_3178, t2b_same_module_optional_param_omitted_22_5) {
    int rc = runSingleModule3178(
        sameModuleProgram3178(mainReturning3178("Opt.WithDefault(1)"), "T2b"),
        "t2b");
    BEZEL_EXPECT_EQ(rc, 8);
}

// The SUPPLIED spelling of the same call. The fix changes how an omitted
// argument is handled; the supplied one must be untouched.
BEZEL_TEST(xmod_call_binding_3178, t2c_imported_optional_param_supplied_22_5) {
    int rc = runImported3178(mainReturning3178("Opt.WithDefault(1, 2)"), "t2c");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 3);
}

// A `string` default and a `bool` default — the other two literal kinds
// `renderConstExpr` exports. `Greet(1)` is 1 + "abc".Length = 4, which cannot
// arise from an empty or zero string; `Flag(10)` is 11 only if the default is
// `true` (a zero-filled `bool` would take the other branch and read 12).
BEZEL_TEST(xmod_call_binding_3178, t2d_imported_string_default_22_5) {
    int rc = runImported3178(mainReturning3178("Opt.Greet(1)"), "t2d");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 4);
}

BEZEL_TEST(xmod_call_binding_3178, t2e_imported_bool_default_22_5) {
    int rc = runImported3178(mainReturning3178("Opt.Flag(10)"), "t2e");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 11);
}

// ===========================================================================
// T3 — §22.5: NAMED arguments bind by NAME across the boundary, so a reordered
// call computes what its single-module twin computes.
//
// THE MISCOMPILE ROW. `Sub(a: int, b: int)` has two formals of the SAME LLVM
// type, so passing them in source order produced valid IR that computed
// `3 - 10 = -7` where the twin computes `10 - 3 = 7`. −7 truncates to 249 as an
// exit status, so the pre-fix and post-fix values are distinguishable here.
// ===========================================================================
BEZEL_TEST(xmod_call_binding_3178, t3_imported_named_args_bind_by_name_22_5) {
    int rc = runImported3178(mainReturning3178("Named.Sub(b := 3, a := 10)"),
                             "t3");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 7);
}

BEZEL_TEST(xmod_call_binding_3178, t3b_same_module_named_args_bind_by_name_22_5) {
    int rc = runSingleModule3178(
        sameModuleProgram3178(mainReturning3178("Named.Sub(b := 3, a := 10)"),
                              "T3b"),
        "t3b");
    BEZEL_EXPECT_EQ(rc, 7);
}

// The mixed-type twin of T3 — pre-fix this one at least FAILED, with malformed
// IR rather than a diagnostic. 5 = 2 + "xyz".Length.
BEZEL_TEST(xmod_call_binding_3178, t3c_imported_named_args_mixed_types_22_5) {
    int rc = runImported3178(
        mainReturning3178("Named.Two(b := \"xyz\", a := 2)"), "t3c");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 5);
}

// A partially-named call (§22.5: "named arguments may appear in any order after
// positional arguments"). 7 = 10 - 3 again, reached by a different binding.
BEZEL_TEST(xmod_call_binding_3178, t3d_imported_positional_then_named_22_5) {
    int rc = runImported3178(mainReturning3178("Named.Sub(10, b := 3)"), "t3d");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 7);
}

// ===========================================================================
// T4 — the INSTANCE-method call paths, `.` and `?.`. Both have their own
// imported-callee argument loop in LlvmBackend_Calls2.cpp, and the `?.` one is
// a separate copy that had the identical source-order placement.
// 101 = 100 + 2 - 1; a source-order placement reads 100 + 1 - 2 = 99.
// ===========================================================================
BEZEL_TEST(xmod_call_binding_3178, t4_imported_instance_named_args_22_5) {
    int rc = runImported3178(
        "  public static method Main(): int "
        "    var i := new Inst(100); "
        "    return i.Add(b := 1, a := 2); "
        "  end Main;",
        "t4");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 101);
}

BEZEL_TEST(xmod_call_binding_3178, t4b_imported_nullcond_named_args_22_5) {
    int rc = runImported3178(
        "  public static method Main(): int "
        "    var i := new Inst(100); "
        "    var j: Inst? := i; "
        "    return j?.Add(b := 1, a := 2) ?? 0; "
        "  end Main;",
        "t4b");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 101);
}

// The `.` form with the optional formal OMITTED: 65 = 100 + 5 - 40.
BEZEL_TEST(xmod_call_binding_3178, t4c_imported_instance_default_omitted_22_5) {
    int rc = runImported3178(
        "  public static method Main(): int "
        "    var i := new Inst(100); "
        "    return i.Add(5); "
        "  end Main;",
        "t4c");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 65);
}

// ===========================================================================
// T5 — §22.5: a named argument that names NO parameter is REJECTED, with the
// same text the single-module twin produces.
//
// THE FALSE-ACCEPT ROW. Pre-fix `One(nosuch := 1)` compiled clean and RAN,
// printing 101 — the argument bound positionally and the misspelt name was
// discarded. A `rejects()`-shaped assertion would not have caught that; the
// value did.
// ===========================================================================
BEZEL_TEST(xmod_call_binding_3178, t5_imported_unknown_param_name_rejected_22_5) {
    expectSameRejection3178(mainReturning3178("Named.One(nosuch := 1)"), "t5",
                            "has no parameter named `nosuch` (\xC2\xA7""22.5)");
}

// §5.1: a named argument of the wrong type is rejected too — pre-fix this
// reached codegen and emitted malformed IR instead of a diagnostic.
BEZEL_TEST(xmod_call_binding_3178, t5b_imported_named_arg_wrong_type_rejected_5_1) {
    expectSameRejection3178(mainReturning3178("Named.One(x := \"nope\")"),
                            "t5b",
                            "is not implicitly convertible to the target type");
}

// §22.5: a parameter bound twice.
BEZEL_TEST(xmod_call_binding_3178, t5c_imported_param_bound_twice_rejected_22_5) {
    expectSameRejection3178(mainReturning3178("Named.Sub(1, a := 2)"), "t5c",
                            "is bound more than once (\xC2\xA7""22.5)");
}

// ===========================================================================
// T6 — the rejections this fix must KEEP. It relaxes an arity rule and admits
// named binding, so the cases that are still errors need their own rows: a
// required (non-defaulted) parameter left unsupplied, and too many arguments.
// Both must read exactly as the single-module twin reads.
// ===========================================================================
BEZEL_TEST(xmod_call_binding_3178, t6_imported_required_param_missing_rejected_22_5) {
    expectSameRejection3178(mainReturning3178("Named.Two(1)"), "t6",
                            "required parameter `b` is not supplied "
                            "(\xC2\xA7""22.5)");
}

BEZEL_TEST(xmod_call_binding_3178, t6b_imported_too_many_args_rejected_22_5) {
    expectSameRejection3178(mainReturning3178("Named.One(1, 2)"), "t6b",
                            "argument(s) but the parameter list has 1 "
                            "(\xC2\xA7""22.5)");
}

// A supplied argument of the wrong type in a plain POSITIONAL call — the check
// that already worked, kept honest while the surrounding code changed.
BEZEL_TEST(xmod_call_binding_3178, t6c_imported_positional_wrong_type_rejected_5_1) {
    expectSameRejection3178(mainReturning3178("Named.One(\"nope\")"), "t6c",
                            "is not implicitly convertible to the target type");
}

// ===========================================================================
// T7 — §22.6's defaulted-arity tie-break (v0.181, #1561) survives ranking
// starting to run across the boundary.
//
// `OverDef.Two(9)` binds BOTH overloads: `Two(a)` exactly, and `Two(a, b := 5)`
// by taking the default. §22.6 prefers the one that fills no default, so the
// answer is 1. Pre-fix the imported form reached this answer by accident —
// `cands` was empty and `arityFallback`'s exact-arity pass picked `Two(a)` —
// so the row is a guard on the WIDENING: now that both are real candidates, the
// tie-break has to do the work the accident used to do.
// ===========================================================================
BEZEL_TEST(xmod_call_binding_3178, t7_imported_defaulted_arity_tiebreak_22_6) {
    int rc = runImported3178(mainReturning3178("OverDef.Two(9)"), "t7");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 1);
}

BEZEL_TEST(xmod_call_binding_3178, t7b_same_module_defaulted_arity_tiebreak_22_6) {
    int rc = runSingleModule3178(
        sameModuleProgram3178(mainReturning3178("OverDef.Two(9)"), "T7b"),
        "t7b");
    BEZEL_EXPECT_EQ(rc, 1);
}

// The two-argument spelling still selects the defaulted overload.
BEZEL_TEST(xmod_call_binding_3178, t7c_imported_two_arg_selects_defaulted_22_6) {
    int rc = runImported3178(mainReturning3178("OverDef.Two(9, 1)"), "t7c");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 2);
}

// ===========================================================================
// T8 — the KNOWN LIMIT, asserted as a LOUD rejection rather than left to
// become a silent codegen bail.
//
// `NonConst.Stamp(a: int, b: int := Named.One(1))` is a legal §22.5 default
// ("call static methods, including factory methods"). Its single-module twin
// compiles and returns 102. `renderConstExpr` cannot export it, so across the
// boundary `hasDefault` is true with an EMPTY `defaultText`: the omission is
// refused, naming the parameter and the follow-up issue. This row is why sema
// and codegen share `sema::importedDefaultTextIsMaterializable` — with sema
// admitting the omission on `hasDefault` alone, this program compiled through
// sema and then abandoned the call in codegen with no diagnostic at all.
//
// The SUPPLIED spelling of the same call must still work (T8b): the limit is
// the omission, not the method.
// ===========================================================================
BEZEL_TEST(xmod_call_binding_3178, t8_imported_nonconstant_default_rejected_22_5) {
    std::string err;
    int rc = runImported3178(mainReturning3178("NonConst.Stamp(1)"), "t8", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, -1);
    BEZEL_EXPECT_TRUE(mentions3178(err, "did not carry as a compile-time value"));
    BEZEL_EXPECT_TRUE(mentions3178(err, "#3213"));
    // Specifically NOT the generic §22.5 arity complaint, which would be a
    // misdescription of a parameter that does declare a default.
    BEZEL_EXPECT_TRUE(!mentions3178(err, "required parameter `b` is not "
                                         "supplied"));
}

BEZEL_TEST(xmod_call_binding_3178, t8b_imported_nonconstant_default_supplied_22_5) {
    int rc = runImported3178(mainReturning3178("NonConst.Stamp(1, 1)"), "t8b");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 2);
}

// The single-module twin of T8 COMPILES and returns 102 — the control that
// makes T8 a statement about the module boundary (and about the metadata
// format) rather than about §22.5 defaults in general.
BEZEL_TEST(xmod_call_binding_3178, t8c_same_module_nonconstant_default_22_5) {
    int rc = runSingleModule3178(
        sameModuleProgram3178(mainReturning3178("NonConst.Stamp(1)"), "T8c"),
        "t8c");
    BEZEL_EXPECT_EQ(rc, 102);
}

}  // namespace bezel::module
