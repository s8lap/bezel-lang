// fix #3176 (§22.3 "Parameter modifiers" / §28.8 "Ownership transfer" /
// §28.14 "Scoped callback parameters" / §2.5 "Dependency resolution"; v0.254)
// — an argument to an IMPORTED METHOD's `owned` / `scoped` formal.
// Family M1 of umbrella #3152; see
// docs/proposals/member-symbol-audit-2026-08-13.md.
//
// WHAT THE DEFECT WAS. #2659 fixed exactly this for the CONSTRUCTOR path and
// wrote its fix in the shape of a shared accessor "so the eventual sweep is
// mechanical; this commit converts only its own site". The sweep did not
// happen. Five more copies of the same walk lived in
// src/ownership/Ownership_Checks.cpp, each spelling
// `dynamic_cast<const ast::MethodDecl*>(ms->decl)` inline. ModuleLoader sets
// `MemberSymbol::decl` only where it re-parsed body text for a GENERIC type, so
// for a non-generic imported class every `MethodSymbol::decl` is null, every
// copy read `params == nullptr`, and every copy answered "this callee has no
// formals at all".
//
// Both halves of §22.3's call-site rule then closed on the user, in opposite
// directions, out of that one null:
//
//   * `Sink.Take(r)` — the UNMARKED spelling reached neither the missing-marker
//     report nor `markMovedByName`, so the transfer was UNRECORDED: the caller
//     still auto-drops `r` at scope end while the callee already disposed it.
//     A DOUBLE FREE.
//
//   * `Sink.Take(owned r)` — the marker was reported spurious ("this parameter
//     position does not take ownership"). A FALSE REJECT.
//
// ⚠ CORRECTED (review of #3176): an earlier draft of this header, and the
// commit body, said "there was no correct way to write the call at all". That
// is FALSE and the review measured it: `Sink.Take(move r)` — the spelling
// v0.254 §22.3 actually mandates, "the caller writes the transfer marker at the
// call site: `M(move x)`" — compiled at b88252b, ran to 211 with exactly one
// disposal, ASan+LSan clean. Only the LEGACY `owned` spelling was
// false-rejected.
//
// The accurate statement is narrower and more interesting: the compiler's own
// diagnostic still emits the v0.169 advice `write 'M(owned r)'`, and THAT
// advice was rejected across a module boundary. A user who followed the
// compiler had no correct way to write the call; a user who followed the spec
// did. Worth keeping precisely because "the tool's guidance disagrees with the
// tool" is a defect class that no test asserts on.
//
// MEASURED PRE-FIX on `main` @ b88252b, with `build/bezelc -c lib.bzl -o lib.o`
// then `build/bezelc --import lib.o app.bzl`:
//
//     unmarked  Take(r)        -> compiles rc=0, runs, **SIGSEGV (139)**
//     under --sanitize=address -> AddressSanitizer: heap-use-after-free,
//                                 READ of size 8 in `main`, freed in
//                                 `bz$OkDep.SinkI$Take`
//     marked    Take(owned r)  -> compile rc=1, the §22.3 spurious-marker error
//     the character-identical single-module twin -> rejected with the §22.3
//                                 "this call consumes `r`" diagnostic
//
// The `scoped` half of the same family is measured too, and it is a pure FALSE
// ACCEPT: `scopedFormalAt` open-coded the same walk with an arity match
// (`d && d->params.size() == n`) that does not merely mis-answer for an
// imported callee — it never MATCHES one — so `checkScopedAdmission`, §28.14's
// capture-set / declared-before / freeze regime, never ran across a module
// boundary at all. A capture whose source is declared AFTER the bounding object
// was rejected in one module and accepted across two: a stored callback
// outliving what it captured.
//
// THE FIX. `CtorFormals` (#2659's three-source view) is now `CalleeFormals`,
// covering a `MethodSymbol` too, read through new `MethodSymbol` overloads of
// `sema::formalParamModifier` / `formalParamName` / `formalParamCount`
// (src/sema/Symbol.hpp, impl in SymbolCollection.cpp) that consult the
// `ast::MethodDecl` / `ast::OperatorDecl` first and the reconstructed
// `ParameterSymbol` list second. One new resolver, `resolveCalleeParams`,
// replaces every open-coded copy, so no two of them can answer differently for
// an imported callee — the #1462 partial state CLAUDE.md warns about.
//
// The `out owned` RECIPIENT pair (`markOutOwnedArgs` /
// `collectCondOutOwnedRecipients`) moves in the same commit, which closes
// #2687(b) and #2659's recorded deferral. ⚠ It could NOT have moved on its
// own, and the order was measured rather than guessed: with the pre-#3186
// `out owned var` handler the recipient local had no `declType` at all, so
// codegen could lower no disposal of it — arming the §28.3 obligation left the
// 16-byte-per-call leak exactly where it was AND added a spurious "borrow
// returned from `made` would outlive its source" reject on the untyped member
// read. #3186 (`src/sema/ExprTypecheck_Exprs.cpp`, the `out owned var` handler
// routed through the both-sources `formalOutParamType2` its plain `out var`
// twin has had since #2657) is a prerequisite, not an adjacent cleanup; T10 and
// T11 pin both halves.
//
// ⚠ ASAN IS NOT THE PRIMARY ORACLE FOR THIS FAMILY. The dependency artifact
// cannot be instrumented (`--sanitize=address` is rejected together with `-c`),
// so the callee's writes and frees are performed by uninstrumented code; #2639
// and #2657 were both reported ASan-CLEAN while SIGSEGV'ing on the plain path.
// Every row below is asserted on the PLAIN build, and disposal is counted with
// a LIVE-OBJECT COUNTER (a static `_disposed` bumped by `Res`'s destructor),
// never with "it didn't crash".
//
// The expected exit code for a running row is `v * 10 + disposals` = **211**:
// `v` is 21 (the callee computes `r.v + 20`, so 21 cannot arise from a skipped
// call, a zeroed field, or a stale read) and the disposal count is exactly 1.
// A double dispose reads 212, a leak 210 — every failure mode this fix touches
// lands on a DIFFERENT value than 211, and all of them fit under the harness's
// 8-bit exit truncation.

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

struct TempFile3176 {
    std::string path;
    explicit TempFile3176(std::string p) : path(std::move(p)) {}
    ~TempFile3176() { std::remove(path.c_str()); }
};

void writeSource3176(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The dependency library.
//   Res   — the counted resource: its destructor bumps a process-global counter
//           the app reads back through GetDisposed(), so "disposed exactly
//           once" is an assertion, not an inference from "no crash".
//   Sink  — `Take` is the METHOD with the `owned` formal (the issue's shape);
//           it computes `r.v + 20` so the observed value proves the callee body
//           ran, then discharges the transferred argument explicitly.
//           `Peek` is the same shape with a BORROW formal: the `owned`
//           call-site marker against it must STILL be rejected, because this
//           fix REMOVES a rejection and the one it must keep needs its own row.
//   Fact  — `Make(out owned r: Res)` is the owned-OUT direction: the callee
//           allocates and the CALLER receives the disposal obligation. It is
//           what the T10 / T11 rows exercise.
//   Wid   — a plain borrowable object for the §28.14 capture rows.
//   Btn   — the §28.14 storage-bounded callback sink: `Set`'s formal is
//           `scoped`, so the caller's captures are checked against `Btn`'s
//           lifetime.
const char* kLib3176 =
    "module OwnMethLib3176; "
    "public class Res "
    "  public static _disposed: int := 0; "
    "  public v: int; "
    "  public constructor() "
    "    v := 1; "
    "  end; "
    "  destructor Res._disposed := Res._disposed + 1; end destructor; "
    "  public static method GetDisposed(): int => Res._disposed; "
    "end Res; "
    "public class Sink "
    "  public static method Take(owned r: Res): int "
    "    var q := r.v + 20; "
    "    r.Dispose(); "
    "    return q; "
    "  end Take; "
    "  public static method Peek(b: Res): int => b.v + 60; "
    "end Sink; "
    "public class Fact "
    "  public static method Make(out owned r: Res): bool "
    "    r := new Res(); "
    "    return true; "
    "  end Make; "
    "end Fact; "
    "public class Wid "
    "  public method Ping() "
    "    Console.WriteLine(\"ping\"); "
    "  end Ping; "
    "end Wid; "
    "public class Btn "
    "  private scoped _h: () -> () := lambda() => (); "
    "  public method Set(scoped h: () -> ()) "
    "    _h := h; "
    "  end Set; "
    "end Btn;";

std::string appSource3176(const std::string& body, const std::string& tag) {
    return "module OwnMethApp3176" + tag + "; import OwnMethLib3176; "
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
int runImported3176(const std::string& body, const std::string& tag,
                    std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3176_" + tag + "_" + unique_stem("");

    TempFile3176 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3176 aObj((dir / (stem + "_a.o")).string());
    writeSource3176(aSrc.path, kLib3176);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile3176 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3176 bExe((dir / (stem + "_b")).string());
    TempFile3176 bErr((dir / (stem + "_b.err")).string());
    writeSource3176(bSrc.path, appSource3176(body, tag));

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

// Compile and run a SINGLE-module program — the differential control twin.
// Same return convention as runImported3176 minus the -2 case.
int runSingleModule3176(const std::string& src, const std::string& tag,
                        std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3176ctl_" + tag + "_" + unique_stem("");
    TempFile3176 sSrc((dir / (stem + ".bzl")).string());
    TempFile3176 sExe((dir / stem).string());
    TempFile3176 sErr((dir / (stem + ".err")).string());
    writeSource3176(sSrc.path, src);

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
        compiled = driver.compileFile(sSrc.path, sExe.path, {}).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(sErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled = driver.compileFile(sSrc.path, sExe.path, {}).success;
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

// The body every "it runs" row shares: build the resource, hand it to the
// imported method, and report `v * 10 + disposals`. The disposal count is read
// in Main, AFTER Work's scope has ended, so a spurious caller-side auto-drop of
// `r` is inside the measured window.
std::string workBody3176(const std::string& callExpr) {
    return "  static method Work(): int "
           "    var r := new Res(); "
           "    return " + callExpr + "; "
           "  end Work; "
           "  public static method Main(): int "
           "    var q := C.Work(); "
           "    return q * 10 + Res.GetDisposed(); "
           "  end Main;";
}

// The single-module twin of kLib3176 plus a body, with no `import` anywhere.
// Character-identical declarations: the ONLY difference from the imported rows
// is the module boundary, which is what makes a divergence a cross-module
// defect rather than a §22.3 / §28.14 enforcement bug.
std::string sameModuleProgram3176(const std::string& body,
                                  const std::string& tag) {
    return "module OwnMethCtl3176" + tag + "; "
           "public class Res "
           "  public static _disposed: int := 0; "
           "  public v: int; "
           "  public constructor() v := 1; end; "
           "  destructor Res._disposed := Res._disposed + 1; end destructor; "
           "  public static method GetDisposed(): int => Res._disposed; "
           "end Res; "
           "public class Sink "
           "  public static method Take(owned r: Res): int "
           "    var q := r.v + 20; r.Dispose(); return q; "
           "  end Take; "
           "  public static method Peek(b: Res): int => b.v + 60; "
           "end Sink; "
           "public class Fact "
           "  public static method Make(out owned r: Res): bool "
           "    r := new Res(); return true; "
           "  end Make; "
           "end Fact; "
           "public class Wid "
           "  public method Ping() Console.WriteLine(\"ping\"); end Ping; "
           "end Wid; "
           "public class Btn "
           "  private scoped _h: () -> () := lambda() => (); "
           "  public method Set(scoped h: () -> ()) _h := h; end Set; "
           "end Btn; "
           "class C " + body + " end C;";
}

bool mentions3176(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ===========================================================================
// T1 — §22.3 / §28.8: the MARKED spelling `Sink.Take(owned r)` on an IMPORTED
// method compiles, the callee body runs, and the resource is disposed EXACTLY
// ONCE.
//
// Pre-fix this was a hard compile error ("the `owned` call-site marker is only
// valid on an argument to an `owned` parameter") — the false-reject half.
// ===========================================================================
BEZEL_TEST(xmod_method_owned_param_3176,
           t1_marked_owned_arg_runs_and_disposes_once_22_3) {
    int rc = runImported3176(workBody3176("Sink.Take(move r)"), "t1");
    if (rc == -2) return;  // no toolchain for the dependency
    BEZEL_EXPECT_EQ(rc, 211);
}

// ===========================================================================
// T2 — §22.3: the UNMARKED spelling `Sink.Take(r)` is REJECTED, naming the fix.
//
// This is the double-free half. Pre-fix it compiled clean and SIGSEGV'd (1011
// here); under `--sanitize=address` the same binary reported
// `heap-use-after-free`. §22.3: "Omitting the marker on a named owned binding
// is a compile error naming the fix." Asserting on the diagnostic TEXT is
// required — the pre-fix outcome was a runtime crash, which a coarse "did it
// reject" check would happily have accepted.
// ===========================================================================
BEZEL_TEST(xmod_method_owned_param_3176, t2_unmarked_owned_arg_rejected_22_3) {
    std::string err;
    int rc = runImported3176(workBody3176("Sink.Take(r)"), "t2", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, -1);  // must NOT build
    BEZEL_EXPECT_TRUE(mentions3176(err, "this call consumes `r`"));
    BEZEL_EXPECT_TRUE(mentions3176(err, "move r"));
    BEZEL_EXPECT_TRUE(mentions3176(err, "§22.3"));
    // And specifically NOT the §22.3 spurious-marker complaint (either the
    // v0.169 `owned`-marker or v0.314 `move`-marker wording).
    BEZEL_EXPECT_TRUE(!mentions3176(err, "does not take ownership"));
}

// ===========================================================================
// T2b — the SAME-MODULE twin of T2 (§2.8.3 rule 4). The two spellings must
// agree; this is the differential control that makes T2 a cross-module claim.
// ===========================================================================
BEZEL_TEST(xmod_method_owned_param_3176,
           t2b_same_module_unmarked_owned_arg_rejected_22_3) {
    std::string err;
    int rc = runSingleModule3176(
        sameModuleProgram3176(workBody3176("Sink.Take(r)"), "T2b"), "t2b",
        &err);
    BEZEL_EXPECT_EQ(rc, -1);
    BEZEL_EXPECT_TRUE(mentions3176(err, "this call consumes `r`"));
    BEZEL_EXPECT_TRUE(mentions3176(err, "§22.3"));
}

// ===========================================================================
// T3 — §22.3 control: a FRESH PRODUCER argument stays UNMARKED and still works.
//
// §22.3 marks only a NAMED owned binding; `Sink.Take(new Res())` has no binding
// to leave moved-from, so no marker is written. This row exists because the fix
// makes an imported `owned` formal visible to the missing-marker report for the
// first time — if that report were keyed on the formal alone rather than on the
// argument being a named binding, this legal shape would become a false reject.
// ===========================================================================
BEZEL_TEST(xmod_method_owned_param_3176, t3_fresh_producer_stays_unmarked_22_3) {
    int rc = runImported3176(
        "  static method Work(): int "
        "    return Sink.Take(new Res()); "
        "  end Work; "
        "  public static method Main(): int "
        "    var q := C.Work(); "
        "    return q * 10 + Res.GetDisposed(); "
        "  end Main;",
        "t3");
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
BEZEL_TEST(xmod_method_owned_param_3176,
           t4_borrow_into_imported_owned_formal_rejected_28_8) {
    std::string err;
    int rc = runImported3176(
        "  static method Give(b: Res): int "
        "    return Sink.Take(move b); "
        "  end Give; "
        "  public static method Main(): int "
        "    var r := new Res(); "
        "    var q := C.Give(r); "
        "    r.Dispose(); "
        "    return q; "
        "  end Main;",
        "t4", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, -1);  // must NOT build
    BEZEL_EXPECT_TRUE(mentions3176(err, "a borrow cannot be passed where an "
                                        "`owned` value is required"));
}

// ===========================================================================
// T5 — §22.3 over-rejection control: the `owned` marker on a BORROW formal of
// an imported METHOD is STILL reported spurious.
//
// The false-reject half of this fix is a REMOVED rejection, so the same
// rejection has to be shown still firing where it is correct. `Peek`'s formal
// is a borrow; §22.3's marker rule does not apply to it, and the diagnostic the
// issue quoted is the RIGHT answer here.
// ===========================================================================
BEZEL_TEST(xmod_method_owned_param_3176,
           t5_marker_on_borrow_formal_still_spurious_22_3) {
    std::string err;
    int rc = runImported3176(
        "  public static method Main(): int "
        "    using var r := new Res(); "
        "    return Sink.Peek(move r); "
        "  end Main;",
        "t5", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, -1);  // must NOT build
    BEZEL_EXPECT_TRUE(mentions3176(
        err, "this parameter does not take ownership"));
}

// ===========================================================================
// T6 — §22.3 over-rejection control: an UNMARKED bare identifier into an
// imported BORROW formal is still ACCEPTED and still reads the right value.
//
// The widest way to get this fix wrong is to treat "the formals resolved" as
// "the formal is owned". `Peek` computes `b.v + 60` = 61 and `r` is disposed by
// its own `using var`, so the value is `61 * 10 + 1` = 611 — which truncates to
// 99 in an 8-bit exit status. Assert the truncated value rather than pretending
// the byte is wider than it is (memory: runet-vs-exit).
// ===========================================================================
BEZEL_TEST(xmod_method_owned_param_3176,
           t6_bare_ident_into_borrow_formal_accepted_22_3) {
    int rc = runImported3176(
        "  static method Work(): int "
        "    using var r := new Res(); "
        "    return Sink.Peek(r); "
        "  end Work; "
        "  public static method Main(): int "
        "    var q := C.Work(); "
        "    return q * 10 + Res.GetDisposed(); "
        "  end Main;",
        "t6");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 611 & 0xFF);
}

// ===========================================================================
// T7 — §28.14: `checkScopedAdmission` now runs for an IMPORTED `scoped` formal.
//
// The capture source `w` is declared AFTER the bounding object `b`, so `w` is
// destroyed FIRST and the stored callback outlives it. One module rejects that;
// two modules accepted it silently, pre-fix, because `scopedFormalAt`'s
// open-coded arity match never matched an imported method at all.
// ===========================================================================
BEZEL_TEST(xmod_method_owned_param_3176,
           t7_imported_scoped_formal_capture_admission_runs_28_14) {
    std::string err;
    int rc = runImported3176(
        "  public static method Main(): int "
        "    using var b := new Btn(); "
        "    using var w := new Wid(); "
        "    b.Set(lambda() => w.Ping()); "
        "    return 0; "
        "  end Main;",
        "t7", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, -1);  // must NOT build
    BEZEL_EXPECT_TRUE(mentions3176(err, "is not declared before the bounding"));
    BEZEL_EXPECT_TRUE(mentions3176(err, "§28.14"));
}

// ===========================================================================
// T7b — the SAME-MODULE twin of T7 (§2.8.3 rule 4 differential control).
// ===========================================================================
BEZEL_TEST(xmod_method_owned_param_3176,
           t7b_same_module_scoped_formal_capture_admission_28_14) {
    std::string err;
    int rc = runSingleModule3176(
        sameModuleProgram3176(
            "  public static method Main(): int "
            "    using var b := new Btn(); "
            "    using var w := new Wid(); "
            "    b.Set(lambda() => w.Ping()); "
            "    return 0; "
            "  end Main;",
            "T7b"),
        "t7b", &err);
    BEZEL_EXPECT_EQ(rc, -1);
    BEZEL_EXPECT_TRUE(mentions3176(err, "is not declared before the bounding"));
    BEZEL_EXPECT_TRUE(mentions3176(err, "§28.14"));
}

// ===========================================================================
// T8 — §28.14 over-rejection control: the ADMISSIBLE capture-free callback into
// the same imported `scoped` formal still compiles and still runs.
//
// T7 turns on a rejection that was silently off. This row is what keeps that
// from becoming a blanket refusal of every imported `scoped` position: a
// capture-free lambda has no captured source to outlive anything, so §28.14
// admits it and the program must build and run to completion (exit 0).
// ===========================================================================
BEZEL_TEST(xmod_method_owned_param_3176,
           t8_imported_scoped_formal_capture_free_accepted_28_14) {
    int rc = runImported3176(
        "  public static method Main(): int "
        "    using var b := new Btn(); "
        "    b.Set(lambda() => Console.WriteLine(\"ok\")); "
        "    return 0; "
        "  end Main;",
        "t8");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 0);
}

// ===========================================================================
// T9 — the SAME-MODULE control for T1: byte-identical declarations and body
// with no `import` must produce the same 211. This pins that the fix converged
// the two paths rather than changing the local one.
// ===========================================================================
BEZEL_TEST(xmod_method_owned_param_3176, t9_same_module_control_22_3) {
    int rc = runSingleModule3176(
        sameModuleProgram3176(workBody3176("Sink.Take(move r)"), "T9"), "t9");
    BEZEL_EXPECT_EQ(rc, 211);
}

// ===========================================================================
// T10 — §22.4 / §28.3 (with #3186): the imported `out owned var` RECIPIENT is
// typed, carries its value, and is disposed EXACTLY ONCE by the §28.3
// auto-drop.
//
// Two defects meet on this one line, and the exit code separates them:
//   * pre-#3186 `made` had NO declType, so `made.v` lowered to a zero behind an
//     `internal: … — #2491` note: `q` = 20, exit 20 * 10 + 0 = **200**;
//   * pre-#3176 `markOutOwnedArgs` read `ms->decl` (null for an import), so the
//     recipient never entered the ownership dataflow and was never dropped:
//     disposals = 0, exit **210**.
// Correct is **211**. Neither fix alone reaches it, which is why they ship
// together.
// ===========================================================================
BEZEL_TEST(xmod_method_owned_param_3176,
           t10_imported_out_owned_recipient_typed_and_dropped_22_4) {
    int rc = runImported3176(
        "  static method Work(): int "
        "    var ok := Fact.Make(out var made); "
        "    if not ok then "
        "      return 0; "
        "    end if; "
        "    return made.v + 20; "
        "  end Work; "
        "  public static method Main(): int "
        "    var q := C.Work(); "
        "    return q * 10 + Res.GetDisposed(); "
        "  end Main;",
        "t10");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 211);
}

// ===========================================================================
// T10b — the SAME-MODULE twin of T10 (§2.8.3 rule 4 differential control).
// ===========================================================================
BEZEL_TEST(xmod_method_owned_param_3176,
           t10b_same_module_out_owned_recipient_22_4) {
    int rc = runSingleModule3176(
        sameModuleProgram3176(
            "  static method Work(): int "
            "    var ok := Fact.Make(out var made); "
            "    if not ok then "
            "      return 0; "
            "    end if; "
            "    return made.v + 20; "
            "  end Work; "
            "  public static method Main(): int "
            "    var q := C.Work(); "
            "    return q * 10 + Res.GetDisposed(); "
            "  end Main;",
            "T10b"),
        "t10b");
    BEZEL_EXPECT_EQ(rc, 211);
}

// ===========================================================================
// T11 — #3186 / §11.3 / §12.1: the imported `out owned var` recipient has a
// REAL type, proven by asking it for a member that does not exist.
//
// T10's 211 could in principle be reached by a recipient typed only well enough
// to lay out a load. This is the sharp row: pre-#3186 `made` had no type at
// all, so `made.Nope` — `Nope` is declared nowhere — compiled CLEAN across the
// boundary while the single-module twin said `no member Nope on type Res`. A
// MISSING diagnostic cannot be caught by an exit code, so it needs its own row
// and an assertion on the text.
// ===========================================================================
BEZEL_TEST(xmod_method_owned_param_3176,
           t11_imported_out_owned_recipient_has_a_real_type_11_3) {
    std::string err;
    int rc = runImported3176(
        "  public static method Main(): int "
        "    var ok := Fact.Make(out var made); "
        "    if not ok then "
        "      return 0; "
        "    end if; "
        "    return made.Nope; "
        "  end Main;",
        "t11", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, -1);  // must NOT build
    BEZEL_EXPECT_TRUE(mentions3176(err, "no member `Nope` on type `Res`"));
}

// ===========================================================================
// T11b — the SAME-MODULE twin of T11 (§2.8.3 rule 4 differential control).
// ===========================================================================
BEZEL_TEST(xmod_method_owned_param_3176,
           t11b_same_module_out_owned_recipient_type_11_3) {
    std::string err;
    int rc = runSingleModule3176(
        sameModuleProgram3176(
            "  public static method Main(): int "
            "    var ok := Fact.Make(out var made); "
            "    if not ok then "
            "      return 0; "
            "    end if; "
            "    return made.Nope; "
            "  end Main;",
            "T11b"),
        "t11b", &err);
    BEZEL_EXPECT_EQ(rc, -1);
    BEZEL_EXPECT_TRUE(mentions3176(err, "no member `Nope` on type `Res`"));
}

}  // namespace bezel::module
