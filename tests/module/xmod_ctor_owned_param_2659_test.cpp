// fix #2659 (§22.3 "Parameter modifiers" / §28.8 "Ownership transfer" /
// §2.5 "Dependency resolution"; v0.237) — an argument to an IMPORTED
// constructor's `owned` parameter.
//
// (Revision note: the checkout these tests were written against carries v0.237
// in `docs/Bezel_Language_Spec.md`'s header; the v0.236->v0.237 diff to that
// file is the version line only, so the §22.3 / §28.8 text quoted below is
// byte-identical at both revisions.)
//
// WHAT THE DEFECT WAS — BOTH failure directions at once, from ONE null.
// `resolveCtorParams` (src/ownership/Ownership_Checks.cpp) resolved a `new
// T(args)` call's formal list by scanning the type's ConstructorSymbols and
// requiring `dynamic_cast<const ast::ConstructorDecl*>(cs->decl)`. That decl is
// NULL for every IMPORTED constructor — ModuleLoader (`MetaMemberKind::
// Constructor`) reconstructs a ConstructorSymbol from dependency metadata with
// no AST at all, keeping the formals only as ParameterSymbols with the
// exporter's modifier string on `ParameterSymbol::modifier`. The `ast::TypeDecl`
// primary-ctor fallback is null cross-module for the same reason. So an imported
// constructor resolved to NOTHING and the ownership pass behaved as though the
// type had no `owned` formals — gating BOTH halves of handleDischarge's
// call-site `owned` logic:
//
//   * `new Imported(owned r)` — the marker was reported spurious: "the `owned`
//     call-site marker is only valid on an argument to an `owned` parameter".
//     A FALSE REJECT of code that is not merely legal but MANDATORY: §22.3
//     states "Omitting the marker on a named owned binding is a compile error
//     naming the fix." The two rules closed on the user and an imported `owned`
//     constructor parameter was uncallable with a named owned binding at all.
//     This is the half the issue was originally filed for.
//
//   * `new Imported(r)` — the UNMARKED spelling reached neither the
//     missing-marker report nor the "bare identifier to an `owned` formal =
//     implicit transfer" `markMovedByName`. It compiled CLEAN with the transfer
//     UNRECORDED: the caller still auto-drops `r` at scope end while the callee
//     also disposes it. A DOUBLE FREE. This half was re-severitied to HIGH on
//     review and is the worse one.
//
// OBSERVED PRE-FIX, on the PLAIN AOT path (both, mechanically):
//     unmarked  `new W7(r)`     -> compiles rc=0, runs, **SIGSEGV (139)**
//     marked    `new W7(move r)` -> compile rc=1, the §22.3 spurious-marker error
//     control, byte-identical code in ONE module -> 211 (correct)
//
// The SIGSEGV was confirmed to be the second disposal, not something adjacent:
// `BEZEL_DUMP_IR=1` on the pre-fix build shows the caller's `Work` emitting,
// AFTER `bz$HLib2659.W7$ctor` (whose body disposes and frees the argument), a
// `using.cls.dispose4` block that reloads the argument's still-non-null slot,
// loads a vtable pointer THROUGH THE FREED BLOCK, calls slot 2 and `free`s it
// again.
//
// THE FIX has two parts, because fixing only the first would have traded the
// false reject for something worse.
//
//   (1) OWNERSHIP (src/ownership/Ownership_Checks.cpp). `resolveCtorParams`
//       now returns a `CtorFormals` that abstracts over the three sources a
//       constructor's formal list can have — a user ConstructorDecl's params, a
//       class PRIMARY constructor's positionalFields (#1627), and an imported
//       ConstructorSymbol's ParameterSymbols — reading the last two through the
//       new `sema::formalParamModifier` / `formalParamName` / `formalParamCount`
//       accessors (src/sema/Symbol.hpp, impl in SymbolCollection.cpp). Those
//       accessors are deliberately written in the shape #2374's structural
//       recommendation proposes for the whole 13-site family, so the eventual
//       sweep is mechanical; this commit converts only its own site.
//
//   (2) CODEGEN (src/codegen/LlvmBackend_Exprs2.cpp). `lowerNew`'s imported
//       branch excluded a by-value `owned` argument from its `allPositional`
//       predicate, so once (1) let `new ImportedT(owned r)` past sema the call
//       hit #2658's loudness guard: NO `bz$T$ctor` emitted, a
//       default-initialized instance, and the argument leaked. Verified — it is
//       what this lane observed with (1) alone applied. The predicate now
//       admits `owned` against an `owned` formal (lowering is identical to a
//       plain positional argument: `owned` is by value), and the call site
//       performs #1918's transfer bookkeeping — `nullTransferredOwnedLocalSlot`
//       at the "call about to run" point — that buildOrderedArgs does for the
//       ordinary path this hand-rolled loop bypasses.
//
// ⚠ ASAN IS NOT AN ORACLE FOR THIS FAMILY. The dependency artifact cannot be
// instrumented (`--sanitize=address` is rejected together with `-c`), so the
// callee's writes and frees are performed by uninstrumented code. A build that
// SIGSEGV'd on the plain path was reported ASan-CLEAN on #2639/#2657. Every row
// below is asserted on the PLAIN build, and disposal is counted with a
// LIVE-OBJECT COUNTER (a static `_disposed` bumped by `Res`'s destructor),
// never with "it didn't crash".
//
// The expected exit code is `v * 10 + disposals` = **211**: `v` is 21 (the
// constructor computes `r.v + 20`, so 21 cannot arise from a skipped
// constructor body, a zeroed field, or a stale read) and the disposal count is
// exactly 1. A double dispose reads 212, a leak 210, a skipped constructor
// body 0/1/2 — every failure mode this fix touches lands on a DIFFERENT value
// than 211, and all of them fit under runAot's 8-bit exit truncation.

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
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile2659 {
    std::string path;
    explicit TempFile2659(std::string p) : path(std::move(p)) {}
    ~TempFile2659() { std::remove(path.c_str()); }
};

void writeSource2659(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The dependency library.
//   Res  — the counted resource: its destructor bumps a process-global counter
//          that the app reads back through GetDisposed(), so "disposed exactly
//          once" is an assertion, not an inference from "no crash".
//   W7   — the constructor with the `owned` formal (the issue's shape). It
//          computes `r.v + 20` so the observed value proves the ctor body ran,
//          then discharges the transferred argument explicitly.
//   WB   — the same shape with a BORROW formal. The `owned` call-site marker
//          against it must STILL be rejected: this fix REMOVES a rejection, so
//          the rejection it must keep needs its own row.
const char* kLib2659 =
    "module OwnCtorLib2659; "
    "public class Res "
    "  public static _disposed: int := 0; "
    "  public v: int; "
    "  public constructor() "
    "    v := 1; "
    "  end; "
    "  destructor Res._disposed := Res._disposed + 1; end destructor; "
    "  public static method GetDisposed(): int => Res._disposed; "
    "end Res; "
    "public class W7 "
    "  public v: int; "
    "  public constructor(owned r: Res) "
    "    v := r.v + 20; "
    "    r.Dispose(); "
    "  end; "
    "end W7; "
    "public class WB "
    "  public v: int; "
    "  public constructor(b: Res) "
    "    v := b.v + 60; "
    "  end; "
    "end WB;";

std::string appSource2659(const std::string& body, const std::string& tag) {
    return "module OwnCtorApp2659" + tag + "; import OwnCtorLib2659; "
           "class C " + body + " end C;";
}

// Compile the dependency, compile the app against it, run the app in a SEPARATE
// process (per tests/README.md). Returns the child's exit status, or:
//   -1 compile/link/run failure (the app did not build)
//   -2 the dependency itself could not be produced (no toolchain)
// `capturedStderr`, when non-null, receives the app compile's stderr — the only
// way to assert on diagnostic TEXT, which a reject row must do (a coarse
// "rejects()" cannot tell a genuine rejection from a runtime SIGSEGV, and this
// defect's pre-fix behaviour was exactly a runtime SIGSEGV).
int runImported2659(const std::string& body, const std::string& tag,
                    std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2659_" + tag + "_" + unique_stem("");

    TempFile2659 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2659 aObj((dir / (stem + "_a.o")).string());
    writeSource2659(aSrc.path, kLib2659);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2659 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2659 bExe((dir / (stem + "_b")).string());
    TempFile2659 bErr((dir / (stem + "_b.err")).string());
    writeSource2659(bSrc.path, appSource2659(body, tag));

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
        compiled = driver.compileFile(bSrc.path, bExe.path, deps).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(bErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled = driver.compileFile(bSrc.path, bExe.path, deps).success;
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

// The body every "it runs" row shares: build the resource, hand it to the
// imported constructor, read the constructed value, discharge the wrapper, and
// report `v * 10 + disposals`. The disposal count is read in Main, AFTER Work's
// scope has ended, so a spurious caller-side auto-drop of `r` is inside the
// measured window.
std::string workBody2659(const std::string& newExpr) {
    return "  static method Work(): int "
           "    var r := new Res(); "
           "    var w := " + newExpr + "; "
           "    var q := w.v; "
           "    _ := w; "
           "    return q; "
           "  end Work; "
           "  public static method Main(): int "
           "    var q := C.Work(); "
           "    return q * 10 + Res.GetDisposed(); "
           "  end Main;";
}

bool mentions2659(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ===========================================================================
// T1 — §22.3 / §28.8: the MARKED spelling `new W7(move r)` compiles, the
// constructor body runs, and the resource is disposed EXACTLY ONCE.
//
// This is the row the issue was filed for (pre-fix: hard compile error, "the
// `owned` call-site marker is only valid on an argument to an `owned`
// parameter"). It is also the row that proves the codegen half: with only the
// ownership half applied this compiled but emitted no `bz$W7$ctor` at all,
// giving v = 0 and a leaked argument (exit 0, not 211).
// ===========================================================================
BEZEL_TEST(xmod_ctor_owned_param_2659, t1_marked_owned_arg_runs_and_disposes_once_22_3) {
    int rc = runImported2659(workBody2659("new W7(move r)"), "t1");
    if (rc == -2) return;  // no toolchain for the dependency
    BEZEL_EXPECT_EQ(rc, 211);
}

// ===========================================================================
// T2 — §22.3: the UNMARKED spelling `new W7(r)` is REJECTED, naming the fix.
//
// This is the double-free half. Pre-fix it compiled clean and SIGSEGV'd (1011
// here). §22.3: "Omitting the marker on a named owned binding is a compile
// error naming the fix." Asserting on the diagnostic TEXT is required — the
// pre-fix outcome was a runtime crash, which a coarse "did it reject" check
// would happily have accepted.
// ===========================================================================
BEZEL_TEST(xmod_ctor_owned_param_2659, t2_unmarked_owned_arg_rejected_22_3) {
    std::string err;
    int rc = runImported2659(workBody2659("new W7(r)"), "t2", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, -1);  // must NOT build
    BEZEL_EXPECT_TRUE(mentions2659(err, "this call consumes `r`"));
    BEZEL_EXPECT_TRUE(mentions2659(err, "move r"));
    BEZEL_EXPECT_TRUE(mentions2659(err, "§22.3"));
    // And specifically NOT the §22.3 spurious-marker complaint (either the
    // v0.169 `owned`-marker or v0.314 `move`-marker wording).
    BEZEL_EXPECT_TRUE(!mentions2659(err, "does not take ownership"));
}

// ===========================================================================
// T3 — §22.3 control: a FRESH PRODUCER argument stays UNMARKED and still works.
//
// §22.3 marks only a NAMED owned binding; `new W7(new Res())` has no binding to
// leave moved-from, so no marker is written. This row exists because the fix
// makes an imported `owned` formal visible to the missing-marker report for the
// first time — if that report were keyed on the formal alone rather than on the
// argument being a named binding, this legal shape would become a false reject.
// ===========================================================================
BEZEL_TEST(xmod_ctor_owned_param_2659, t3_fresh_producer_stays_unmarked_22_3) {
    std::string err;
    int rc = runImported2659(
        "  static method Work(): int "
        "    var w := new W7(new Res()); "
        "    var q := w.v; "
        "    _ := w; "
        "    return q; "
        "  end Work; "
        "  public static method Main(): int "
        "    var q := C.Work(); "
        "    return q * 10 + Res.GetDisposed(); "
        "  end Main;",
        "t3", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 211);
}

// ===========================================================================
// T4 — §28.8 control: a BORROW passed with `owned` to the imported `owned`
// formal is still REJECTED.
//
// The fix widens what counts as an owned sink across a module boundary, so the
// rejection that keeps a borrow OUT of that sink must be shown to survive:
// admitting it would be the double free this issue is about, merely relocated.
// `b` is a plain (borrow) parameter of the enclosing method.
// ===========================================================================
BEZEL_TEST(xmod_ctor_owned_param_2659, t4_borrow_into_imported_owned_formal_rejected_28_8) {
    std::string err;
    int rc = runImported2659(
        "  static method Take(b: Res): int "
        "    var w := new W7(move b); "
        "    var q := w.v; "
        "    _ := w; "
        "    return q; "
        "  end Take; "
        "  public static method Main(): int "
        "    var r := new Res(); "
        "    var q := C.Take(r); "
        "    r.Dispose(); "
        "    return q; "
        "  end Main;",
        "t4", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, -1);  // must NOT build
    BEZEL_EXPECT_TRUE(!err.empty());
}

// ===========================================================================
// T5 — §22.3 control: the `owned` marker on a BORROW formal of an imported
// constructor is STILL reported spurious.
//
// The false-reject half of this fix is a REMOVED rejection, so the same
// rejection has to be shown still firing where it is correct. `WB`'s formal is
// a borrow; §22.3's marker rule does not apply to it, and the diagnostic the
// issue quoted is the RIGHT answer here.
// ===========================================================================
BEZEL_TEST(xmod_ctor_owned_param_2659, t5_marker_on_borrow_formal_still_spurious_22_3) {
    std::string err;
    int rc = runImported2659(
        "  static method Work(): int "
        "    var r := new Res(); "
        "    var w := new WB(move r); "
        "    var q := w.v; "
        "    _ := w; "
        "    r.Dispose(); "
        "    return q; "
        "  end Work; "
        "  public static method Main(): int => C.Work();",
        "t5", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, -1);  // must NOT build
    BEZEL_EXPECT_TRUE(mentions2659(
        err, "this parameter does not take ownership"));
}

// ===========================================================================
// T6 — the SAME-MODULE control: byte-identical declarations and Main body with
// no `import` must produce the same 211. This is what made the defect a
// cross-module one rather than a §22.3 enforcement bug, and it pins that the
// fix converged the two paths rather than changing the local one.
// ===========================================================================
BEZEL_TEST(xmod_ctor_owned_param_2659, t6_same_module_control_22_3) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2659_t6_");
    TempFile2659 src((dir / (stem + ".bzl")).string());
    TempFile2659 exe((dir / stem).string());
    writeSource2659(
        src.path,
        std::string("module OwnCtorCtl2659; ") +
            "public class Res "
            "  public static _disposed: int := 0; "
            "  public v: int; "
            "  public constructor() v := 1; end; "
            "  destructor Res._disposed := Res._disposed + 1; end destructor; "
            "  public static method GetDisposed(): int => Res._disposed; "
            "end Res; "
            "public class W7 "
            "  public v: int; "
            "  public constructor(owned r: Res) "
            "    v := r.v + 20; r.Dispose(); "
            "  end; "
            "end W7; "
            "class C " + workBody2659("new W7(move r)") + " end C;");

    bezel::Driver driver;
    if (!driver.compileFile(src.path, exe.path, {}).success) {
        BEZEL_EXPECT_TRUE(false);
        return;
    }
    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(exe.path.c_str(), exe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    BEZEL_EXPECT_TRUE(WIFEXITED(status));
    if (WIFEXITED(status)) BEZEL_EXPECT_EQ(WEXITSTATUS(status), 211);
}

}  // namespace bezel::module
