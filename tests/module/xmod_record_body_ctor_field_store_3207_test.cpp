// fix #3207 (§9.2 "Records" / §2.5 "Cross-module symbols", v0.254) — an
// IMPORTED body record's authored constructor RUNS but LOSES FIELD STORES:
// every body-storage member (auto-property) reads back its §7.9 default.
// Silent wrong value, no diagnostic at any severity. A positional record
// constructed through the same `new R(args)` form in the same artifact
// round-trips correctly, so the defect is specific to a body record reached
// through an authored constructor.
//
// WHY THIS IS HIGH-SEVERITY. The ctor body executes — the `Console.WriteLine`
// inside the ctor fires with the right argument — so the false-value read
// looks like an unrelated runtime oddity unless the test pins the exact
// value. A validating constructor's normalization (§9.2's own `record
// Rectangle` example) is silently discarded; an "I just stored a positive
// integer and now it reads zero" complaint has no source-anchored trace.
//
// WHERE THE BUG WAS. ModuleLoader::parseAndAttachCtorBody (§14/#662c, §22.7)
// re-parses the exporter's `ctorBodyText` over a synthetic single-type source
// to obtain a live `ast::ConstructorDecl` for codegen's
// buildRecordValue-inline path. The synthetic source includes Field and
// Method stubs (so the re-parse resolves `this._field` and self-method
// calls) but NOT Property stubs — so a ctor body that names a body-storage
// auto-property (`this.X := x;`) failed sema with "no member `X` on type
// `BodyRec`". The fresh PropertySymbol binding was never created; the remap
// loop below therefore had nothing to remap; codegen's
// `ctx_.exprSymbol(this.X)` returned null in the AST-free fallback path,
// and the body store silently dropped the write.
//
// The same `parseAndAttachCtorBody` remap loop handles Field, Parameter,
// Method and Local symbols but had no PropertySymbol arm. The fix is two
// parts:
//   1. buildSyntheticClassSource emits a `public [readonly] property NAME: T;`
//      stub for every MetaMember of kind Property (mirroring the Field stub
//      already there). A computed property's accessor bodies are NOT carried
//      — the re-parse cannot faithfully re-resolve sibling-type references
//      (#710/#2290) — but the bare declaration is enough for a ctor body to
//      NAME the property; reads dispatch through the exported getter stub
//      (already in place), writes are gated by `readonly` upstream.
//   2. The fresh PropertySymbol created from that stub is remapped to the
//      IMPORTED PropertySymbol so codegen's `storageMemberOf` /
//      `fieldIndex_` / `recordStruct` are keyed on the imported type — the
//      same struct whose `slot` is being filled — rather than on the
//      synthetic re-parse type, whose LLVM StructType is a separate identity
//      that fieldAddr built on the fly. Gated on `mm.isAutoProperty` (the
//      wire bit #2478 carries) so a COMPUTED property's accessor body keeps
//      its fresh symbol and the existing inlined-body path.
//
// WHAT THE DEFECT WAS, measured on HEAD (`b2e1bf3`) before this fix, with a
// two-module `bezelc -c lib.bzl -o lib.o` + `bezelc --import lib.o app.bzl`
// AOT pair (the strict oracle):
//
//   module BrLib;
//   public record BodyRec
//     public readonly property X: int;
//     public constructor(x: int)
//       Console.WriteLine("ctor ran with " + x.ToString());
//       this.X := x;             -- BODY STORE; landed as zero pre-fix
//     end constructor;
//   end BodyRec;
//   public record PosRec(A: int);
//
//   module BrApp; import BrLib;
//   public class C
//     public static method Main(): int
//       var r := new BodyRec(7);
//       Console.WriteLine("read back " + r.X.ToString());   -- "read back 0" pre-fix
//       var p := new PosRec(5);
//       Console.WriteLine("pos read back " + p.A.ToString());
//       return 0;
//     end Main;
//   end C;
//
//   IMPORTED (pre-fix) -> exit 0
//     ctor ran with 7
//     read back 0            <-- WRONG (should be 7)
//     pos read back 5
//   SINGLE-MODULE TWIN (pre-fix) -> exit 0
//     ctor ran with 7
//     read back 7            <-- correct
//     pos read back 5
//
// Post-fix both rows produce "read back 7". The only difference between the
// two programs is the module boundary, which makes a divergence a
// cross-module defect rather than a §9.2 enforcement bug.
//
// ⚠ WHY THE ROWS ASSERT ON VALUE, NOT ON "did it compile". The accept rows
// pin the literal program output. A row that only checked `rc == 0` would
// pass on a crash-with-zero-exit or on an unrelated accept.
//
// ⚠ THE PIN IS VALUE-ONLY; STDERR IS NOT PINNED. The defect is silent — no
// diagnostic at any severity — so a stderr check is vacuous. The value pin
// is the only test the defect class admits.
//
// ⚠ T1 / T1b IS THE DIFFERENTIAL CONTROL. The pair pins "the importer agrees
// with the twin" — T1 alone would pass on a compiler that always emits 7,
// T1b alone would pass on a §9.2 enforcement bug. Pairs together pin the
// cross-module claim.
//
// ⚠ T2 EXTENDS TO MULTIPLE BODY-STORAGE MEMBERS. T1 covers a single
// auto-property. T2 covers two, exercising that the fieldIndex_ map (one
// entry per body-storage member, in declaration order) is populated
// correctly for every member the ctor body writes, not just the first one.
// A regression where only the first slot mapped would pass T1 and fail T2.
//
// ⚠ T3 IS THE INIT+STORE SHAPE. A ctor body that READS the auto-property's
// declared initializer via the auto-store replay (buildRecordValue's
// decl-null constant-foldable initializer fallback, #2467) and then
// OVERWRITES it via `this.X := v` — exercises the path where buildRecordValue
// first seeds the slot from `ctx.importedConstValue(m)` (#2467) and the
// inlined ctor body then writes through the remapped `this.X`. Pre-fix the
// initial seed was overridden by the inlined body's drop; the
// post-init-readback value should be the AUTHORED value, not the initializer.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // #3156: the ONE sanitizer-honouring compileFile wrapper
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile3207 {
    std::string path;
    explicit TempFile3207(std::string p) : path(std::move(p)) {}
    ~TempFile3207() { std::remove(path.c_str()); }
};

void writeSource3207(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct RunResult3207 {
    int rc;
    std::string stdout_text;
};

// Compile the dependency, compile the app against it, run the app in a
// SEPARATE process (per tests/README.md). Returns the child's exit status and
// captured stdout text — the only way to assert on program VALUE.
//   -1 compile/link/run failure (the app did not build)
//   -2 the dependency itself could not be produced (no toolchain)
RunResult3207 runImported3207(const std::string& libSrc,
                              const std::string& appSrc,
                              const std::string& tag) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3207_" + tag + "_" + std::to_string(std::rand());

    TempFile3207 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3207 aObj((dir / (stem + "_a.o")).string());
    writeSource3207(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return {-2, ""};
    }
    if (!std::ifstream(aObj.path).good()) return {-2, ""};

    TempFile3207 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3207 bExe((dir / (stem + "_b")).string());
    writeSource3207(bSrc.path, appSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bool compiled = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path,
                                                   deps).success;
    if (!compiled) return {-1, ""};
    if (!std::ifstream(bExe.path).good()) return {-1, ""};

    // Run the app, capturing stdout into a pipe.
    int pipefd[2];
    if (::pipe(pipefd) != 0) return {-1, ""};
    pid_t pid = ::fork();
    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::close(pipefd[1]);
        ::execl(bExe.path.c_str(), bExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    ::close(pipefd[1]);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0)
        out.append(buf, buf + n);
    ::close(pipefd[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return {1000 + WTERMSIG(status), out};
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, out};
}

// Compile and run a SINGLE-module program — the differential control twin.
RunResult3207 runSingleModule3207(const std::string& src,
                                  const std::string& tag) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3207ctl_" + tag + "_" + std::to_string(std::rand());
    TempFile3207 sSrc((dir / (stem + ".bzl")).string());
    TempFile3207 sExe((dir / stem).string());
    writeSource3207(sSrc.path, src);
    bezel::Driver driver;
    bool compiled = bezel::test::compileFileForTest(driver, sSrc.path, sExe.path,
                                                   {}).success;
    if (!compiled) return {-1, ""};
    if (!std::ifstream(sExe.path).good()) return {-1, ""};

    int pipefd[2];
    if (::pipe(pipefd) != 0) return {-1, ""};
    pid_t pid = ::fork();
    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::close(pipefd[1]);
        ::execl(sExe.path.c_str(), sExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    ::close(pipefd[1]);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0)
        out.append(buf, buf + n);
    ::close(pipefd[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return {1000 + WTERMSIG(status), out};
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, out};
}

// The dependency library: a body record with one auto-property whose
// authored ctor stores the argument into that property. Plus a positional
// control record whose field round-trips — the same artifact proves the
// defect is body-record-specific, not a generic cross-module loss.
const char* kLib3207 =
    "module BrLib3207; "
    "public record BodyRec "
    "  public readonly property X: int; "
    "  public constructor(x: int) "
    "    Console.WriteLine(\"ctor ran with \" + x.ToString()); "
    "    this.X := x; "
    "  end constructor; "
    "end BodyRec; "
    "public record PosRec(A: int);";

const char* kLibTwoField3207 =
    "module BrLib3207Two; "
    "public record TwoRec "
    "  public readonly property A: int; "
    "  public readonly property B: int; "
    "  public constructor(a: int, b: int) "
    "    this.A := a; "
    "    this.B := b; "
    "  end constructor; "
    "end TwoRec;";

const char* kLibInit3207 =
    "module BrLib3207Init; "
    "public record InitRec "
    "  public readonly property X: int := 100; "
    "  public constructor(x: int) "
    "    this.X := x; "
    "  end constructor; "
    "end InitRec;";

const char* kLocalDecls3207 =
    "public record BodyRec "
    "  public readonly property X: int; "
    "  public constructor(x: int) "
    "    Console.WriteLine(\"ctor ran with \" + x.ToString()); "
    "    this.X := x; "
    "  end constructor; "
    "end BodyRec; "
    "public record PosRec(A: int);";

const char* kLocalTwoDecls3207 =
    "public record TwoRec "
    "  public readonly property A: int; "
    "  public readonly property B: int; "
    "  public constructor(a: int, b: int) "
    "    this.A := a; "
    "    this.B := b; "
    "  end constructor; "
    "end TwoRec;";

const char* kLocalInitDecls3207 =
    "public record InitRec "
    "  public readonly property X: int := 100; "
    "  public constructor(x: int) "
    "    this.X := x; "
    "  end constructor; "
    "end InitRec;";

std::string appSource3207(const std::string& body, const std::string& tag) {
    return "module BrApp3207" + tag + "; import BrLib3207; " + body;
}

std::string appSourceTwo3207(const std::string& body, const std::string& tag) {
    return "module BrApp3207" + tag + "; import BrLib3207Two; " + body;
}

std::string appSourceInit3207(const std::string& body, const std::string& tag) {
    return "module BrApp3207" + tag + "; import BrLib3207Init; " + body;
}

std::string sameModuleProgram3207(const std::string& body, const std::string& tag) {
    return "module Ctl3207" + tag + "; " + std::string(kLocalDecls3207) + " " + body;
}

std::string sameModuleProgramTwo3207(const std::string& body,
                                     const std::string& tag) {
    return "module Ctl3207" + tag + "; " + std::string(kLocalTwoDecls3207) + " " + body;
}

std::string sameModuleProgramInit3207(const std::string& body,
                                      const std::string& tag) {
    return "module Ctl3207" + tag + "; " + std::string(kLocalInitDecls3207) + " " + body;
}

// std::string findAll3207(const std::string& hay, const std::string& needle) —
// helper asserting that stdout CONTAINS the expected substring.
bool contains3207(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ===========================================================================
// T1 — HEADLINE ROW: an IMPORTED body record's authored ctor stores its
// argument into the record's auto-property, and the post-construction read
// of that property returns the stored value (not the §7.9 default).
//
// Pre-fix this exited 0 and printed "read back 0" while the ctor body's
// `Console.WriteLine("ctor ran with 7")` ran with the right argument —
// silent loss of the field store. Post-fix "read back 7".
//
// The positional control record's "pos read back 5" row is the same
// artifact's reference point: it confirms the ctor-call / read-back shape
// works for a positional field, so the body-record case is the actual defect
// (and not a generic cross-module record-construction regression).
// ===========================================================================
BEZEL_TEST(xmod_record_body_ctor_field_store_3207,
           t1_imported_body_record_ctor_field_store_lands_9_2) {
    RunResult3207 r = runImported3207(
        kLib3207,
        appSource3207(
            "public class C3207T1 "
            "  public static method Main(): int "
            "    var r := new BodyRec(7); "
            "    Console.WriteLine(\"read back \" + r.X.ToString()); "
            "    var p := new PosRec(5); "
            "    Console.WriteLine(\"pos read back \" + p.A.ToString()); "
            "    return 0; "
            "  end Main; "
            "end C3207T1;",
            "T1"),
        "t1");
    if (r.rc == -2) return;  // no toolchain for the dependency
    BEZEL_EXPECT_EQ(r.rc, 0);
    BEZEL_EXPECT_TRUE(contains3207(r.stdout_text, "ctor ran with 7"));
    BEZEL_EXPECT_TRUE(contains3207(r.stdout_text, "read back 7"));
    BEZEL_EXPECT_TRUE(contains3207(r.stdout_text, "pos read back 5"));
}

// ===========================================================================
// T1b — the SAME-MODULE twin of T1. Both halves of the pair are required:
// T1 alone would pass on a compiler that always emits 7; T1b alone would
// pass on a §9.2 enforcement bug rather than a cross-module claim. Together
// they pin "the importer agrees with the twin".
// ===========================================================================
BEZEL_TEST(xmod_record_body_ctor_field_store_3207,
           t1b_same_module_body_record_ctor_field_store_lands_9_2) {
    RunResult3207 r = runSingleModule3207(
        sameModuleProgram3207(
            "public class C3207T1b "
            "  public static method Main(): int "
            "    var r := new BodyRec(7); "
            "    Console.WriteLine(\"read back \" + r.X.ToString()); "
            "    var p := new PosRec(5); "
            "    Console.WriteLine(\"pos read back \" + p.A.ToString()); "
            "    return 0; "
            "  end Main; "
            "end C3207T1b;",
            "T1b"),
        "t1b");
    BEZEL_EXPECT_EQ(r.rc, 0);
    BEZEL_EXPECT_TRUE(contains3207(r.stdout_text, "ctor ran with 7"));
    BEZEL_EXPECT_TRUE(contains3207(r.stdout_text, "read back 7"));
    BEZEL_EXPECT_TRUE(contains3207(r.stdout_text, "pos read back 5"));
}

// ===========================================================================
// T2 — TWO-PROPERTY EXTENSION. A body record with two auto-properties whose
// authored ctor stores into both, in source order. Exercises that the
// `fieldIndex_` map is populated correctly for every member the ctor body
// writes — a regression where only the first slot mapped would pass T1 and
// fail T2 (the second field's store would land at offset 0 too, returning
// the first argument's value where the second is expected).
// ===========================================================================
BEZEL_TEST(xmod_record_body_ctor_field_store_3207,
           t2_imported_two_property_record_both_field_stores_land) {
    RunResult3207 r = runImported3207(
        kLibTwoField3207,
        appSourceTwo3207(
            "public class C3207T2 "
            "  public static method Main(): int "
            "    var r := new TwoRec(11, 22); "
            "    Console.WriteLine(\"a=\" + r.A.ToString()); "
            "    Console.WriteLine(\"b=\" + r.B.ToString()); "
            "    return 0; "
            "  end Main; "
            "end C3207T2;",
            "T2"),
        "t2");
    if (r.rc == -2) return;
    BEZEL_EXPECT_EQ(r.rc, 0);
    BEZEL_EXPECT_TRUE(contains3207(r.stdout_text, "a=11"));
    BEZEL_EXPECT_TRUE(contains3207(r.stdout_text, "b=22"));
}

BEZEL_TEST(xmod_record_body_ctor_field_store_3207,
           t2b_same_module_two_property_record_both_field_stores_land) {
    RunResult3207 r = runSingleModule3207(
        sameModuleProgramTwo3207(
            "public class C3207T2b "
            "  public static method Main(): int "
            "    var r := new TwoRec(11, 22); "
            "    Console.WriteLine(\"a=\" + r.A.ToString()); "
            "    Console.WriteLine(\"b=\" + r.B.ToString()); "
            "    return 0; "
            "  end Main; "
            "end C3207T2b;",
            "T2b"),
        "t2b");
    BEZEL_EXPECT_EQ(r.rc, 0);
    BEZEL_EXPECT_TRUE(contains3207(r.stdout_text, "a=11"));
    BEZEL_EXPECT_TRUE(contains3207(r.stdout_text, "b=22"));
}

// ===========================================================================
// T3 — INIT+STORE SHAPE. A body record with a declared auto-property
// initializer AND an authored ctor that overwrites it. buildRecordValue
// seeds the slot from `ctx.importedConstValue(m)` (the wire-carried
// initializer text, #2467) and the inlined ctor body then writes through
// the remapped `this.X`. Pre-fix the initial seed was overridden by the
// inlined body's drop, but the seed also wasn't visible. Post-fix the
// post-construction readback is the AUTHORED value (42), not the initializer
// (100), and not the default (0).
//
// A subtle ordering note: this row exercises the path where the ctor body
// runs AFTER the initializer replay — the initializer replay is hoisted
// ahead of the ctor body in buildRecordValue (line 6990 area). So `r.X`
// must read 42, the ctor-arg value. If this row ever reads 100, the ctor
// body is being skipped entirely; if it reads 0, the initializer replay is
// being overwritten by the body drop (the pre-fix behavior at one remove).
// ===========================================================================
BEZEL_TEST(xmod_record_body_ctor_field_store_3207,
           t3_imported_init_then_ctor_store_overrides_initializer) {
    RunResult3207 r = runImported3207(
        kLibInit3207,
        appSourceInit3207(
            "public class C3207T3 "
            "  public static method Main(): int "
            "    var r := new InitRec(42); "
            "    Console.WriteLine(\"r.X=\" + r.X.ToString()); "
            "    return 0; "
            "  end Main; "
            "end C3207T3;",
            "T3"),
        "t3");
    if (r.rc == -2) return;
    BEZEL_EXPECT_EQ(r.rc, 0);
    BEZEL_EXPECT_TRUE(contains3207(r.stdout_text, "r.X=42"));
}

BEZEL_TEST(xmod_record_body_ctor_field_store_3207,
           t3b_same_module_init_then_ctor_store_overrides_initializer) {
    RunResult3207 r = runSingleModule3207(
        sameModuleProgramInit3207(
            "public class C3207T3b "
            "  public static method Main(): int "
            "    var r := new InitRec(42); "
            "    Console.WriteLine(\"r.X=\" + r.X.ToString()); "
            "    return 0; "
            "  end Main; "
            "end C3207T3b;",
            "T3b"),
        "t3b");
    BEZEL_EXPECT_EQ(r.rc, 0);
    BEZEL_EXPECT_TRUE(contains3207(r.stdout_text, "r.X=42"));
}

}  // namespace bezel::module
