// Spec-derived regression test for issue #2301 — a boxed value type loses its
// identity across a `--import` boundary (`o is int` returns false when the box
// was made in another module).
//
//   §8.6 (boxing, v0.228+),
//   §28.9 (boxed value types are runtime-managed),
//   §2.8.3 rule 4 (program-wide type identity: `is`/`as`/pattern matching over
//     values of a type match identically regardless of which artifact produced
//     the value or performs the test).
//
// The defect (§2.8.3 rule 4 violation): the §8.6 box vtable `pvt$<prim>` is
// emitted with `InternalLinkage`, so every compilation unit mints its OWN
// `pvt$Int` at a distinct address. A box carries word-0 = `pvt$Int_<producer>`,
// and `bz_obj_is_string(obj, vtable_table)` checks word-0 against the LOCAL
// `bz$vtable$table` (a per-module `InternalLinkage` array listing the local
// module's vtables only). For a box the producer module made, the local table
// does not contain `pvt$Int_<producer>`; the discriminator returns "is a
// string", `emitIsA` is gated off, and `o is int` answers false even though the
// descriptor chain (now `LinkOnceODR` per the partial fix on `ptd$<prim>`) is
// shared across modules.
//
// The fix: `pvt$<prim>` (and the thunks its initializer references — see
// `LlvmBackend_Setup.cpp`'s `primBox{Equals,Hash,ToString}Thunk` and
// `boxDefault{Equals,HashCode}Thunk` + `builtinDisposeFn_`) are emitted with
// `LinkOnceODR` linkage instead, ODR-mergeable across modules because their
// bodies are deterministic per primitive (the runtime helpers they call have
// `ExternalLinkage`). One address per box vtable program-wide → both modules'
// `bz$vtable$table`s list that merged address → `bz_obj_is_string` correctly
// identifies cross-module boxes as not-string → `emitIsA` walks the merged
// `ptd$<prim>` chain → match.
//
// TEST COVERAGE (the axis grid):
//
// T1 — HEADLINE REPRO (issue #2301 verbatim): `LibX.Api.MakeBox/TakeBox` boxes
//      an `int` in one module and tests it in another. Pre-fix: the cross-
//      module `is int` returns `0|1|0`. Post-fix: `1|1|1`.
// T2 — IN-MODULE CONTROL: the same shape entirely within one compilation.
//      Pre-fix: `1|1|1` (works; the issue's third column is the in-module arm
//      that the user included as the "working baseline"). Post-fix: `1|1|1`.
// T3 — PRIMITIVE FAMILY SWEEP: `long`, `bool`, `double`, `decimal` instead of
//      `int`. Pre-fix: each cross-module `is <prim>` returns `0` (same bug
//      across the entire BZ_BOX_PRIMITIVES list). Post-fix: each returns `1`.
//
// SCOPE NOTE: the same defect class also affects boxed RECORDS (the §8.6
// `bvt$box$<RecordName>` vtable, set up by `boxVtable_` for `record Pt(X: int,
// Y: int)`, etc.) — the discriminator gate fails identically for a cross-
// module boxed record's `is Pt`. The record-side fix needs the same
// LinkOnceODR cascade over `boxVtable_[ts]` + `boxRecordEqualsThunk` +
// `boxRecordHashThunk` + `boxToStringThunk` + the per-record itmap/itables;
// that is the natural follow-up lane (#2301 part 2), but its blast radius is
// wider and is not what this lane is scoped to.
//
// ORACLE: the AOT fork-isolated `runCapture` (mirroring
// `xmod_ctor_named_arg_defaults_3268_test.cpp`'s build pattern) — a wrong-value
// defect cannot survive: the binary either prints the spec-stated values or it
// doesn't. The expected output string is the load-bearing pin (an 8-bit exit
// code cannot carry three columns of `0|1|1`-style data; stdout text is).
//
// ── WHY A REGRESSION TEST SOURCE THAT COMPILES CLEAN ───────────────────
//
// The 2026-08-22 earned rule "regression test source syntax can be the bug"
// applies — each `.T` row uses Bezel syntax that is legal at HEAD v0.278 (the
// spec revision this repo's docs header reports). The repro code compiles
// without diagnostic in any well-formed Bezel program.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"


namespace bezel::codegen {

namespace {

const std::filesystem::path& wtDir2301() {
    return wtScratchDir("wt-2301");
}

struct TempFile {
    std::string path;
    explicit TempFile(std::string p) : path(std::move(p)) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct RunResult { int code; std::string out; };

RunResult runCapture(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

struct LibHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle buildLib(const std::filesystem::path& dir,
                   const std::string& tag,
                   const std::string& libSrc) {
    LibHandle h;
    std::string stem = unique_stem(("bezel_2301lib_" + tag + "_").c_str());
    h.src = std::make_shared<TempFile>((dir / (stem + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile>((dir / (stem + "_lib.o")).string());
    writeSource(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

struct InProcessApp {
    bool ok = false;
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> exe;
    std::string exePath;
};

InProcessApp buildApp(const std::filesystem::path& dir,
                      const std::string& tag,
                      const std::string& libObjPath,
                      const std::string& appSrc) {
    InProcessApp r;
    if (libObjPath.empty()) return r;
    std::string stem = unique_stem(("bezel_2301app_" + tag + "_").c_str());
    r.src = std::make_shared<TempFile>((dir / (stem + "_app.bzl")).string());
    r.exe = std::make_shared<TempFile>((dir / (stem + "_app")).string());
    writeSource(r.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d; d.metadataPath = libObjPath; d.objectPath = libObjPath;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(r.src->path, r.exe->path, deps);
    r.ok = rb.success;
    r.exePath = r.exe->path;
    return r;
}

}  // namespace

// ── (T1) HEADLINE REPRO — §8.6 / §28.9 / §2.8.3 rule 4 ────────────────────
//
// `LibX.Api.MakeBox/TakeBox` boxes an `int` in module LibX; `App.Main` imports
// LibX, builds a `var o: Object := 5` (its own box), passes it through
// `Api.TakeBox` (which is A testing B's box) AND calls `Api.MakeBox(7)` (which
// is B testing A's box), AND tests its own box directly. Expected: every
// `is int` returns `1`, so the printed triplet is `1|1|1`. Pre-fix:
// `0|1|0` (the B-tests-A and A-tests-B columns fail; the same-module column
// is unaffected).
//
// A box in module A carries word-0 = `pvt$Int_<A>` (InternalLinkage in A).
// Module B's `bz$vtable$table` lists `pvt$Int_<B>`, not A's. `bz_obj_is_string`
// answers "is a string" → `emitIsA` gated off → `is int` returns false.
// Post-fix: `pvt$Int` LinkOnceODR merges at link time → both tables contain
// the merged address → `bz_obj_is_string` answers "not a string" → `emitIsA`
// walks the (already-merged) `ptd$Int` chain → match.
BEZEL_TEST(xmod_boxed_primitive_is_int_across_import_2301,
           headline_repro_spec_8_6_28_9_2_8_3) {
    auto dir = wtDir2301();
    const std::string lib =
        "module WTLib2301_t1; "
        "public class Api "
        "  public constructor() end constructor; "
        "  public static method MakeBox(n: int): Object return n; end MakeBox; "
        "  public static method TakeBox(o: Object): int "
        "    return (if (o is int) then 1 else 0); "
        "  end TakeBox; "
        "  destructor end destructor; "
        "end Api; ";
    const std::string app =
        "module WTApp2301_t1; "
        "import WTLib2301_t1; "
        "public class Program "
        "  public static method Main(): int "
        "    var n := 5; "
        "    var o: Object := n; "
        "    var inA := Api.TakeBox(o); "
        "    var inB := (if (o is int) then 1 else 0); "
        "    var fromA := Api.MakeBox(7); "
        "    var backInB := (if (fromA is int) then 1 else 0); "
        "    Console.WriteLine(inA.ToString() + \"|\" + inB.ToString() + \"|\" + backInB.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    LibHandle libH = buildLib(dir, "t1", lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, "t1", libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    // The load-bearing pin: stdout text. The exit code is 0 either way (the
    // program returns 0 unconditionally); the bug surfaces ONLY in the
    // printed `1|1|1` vs `0|1|0` triplet. Trim trailing whitespace that the
    // println may add.
    std::string trimmed = out;
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r' || trimmed.back() == ' '))
        trimmed.pop_back();
    BEZEL_EXPECT_EQ(trimmed, std::string("1|1|1"));
}

// ── (T2) IN-MODULE CONTROL — same program, no import ───────────────────────
//
// Same logic as T1 but EVERYTHING in one module: A's box and B's `is int`
// share a compilation. Pre-fix: prints `1|1|1` (the issue's "in-module
// baseline"). Post-fix: still `1|1|1`. Pins that the cross-module fix does
// not regress the in-module path that the partial `ptd$<prim>` LinkOnceODR
// already covered.
BEZEL_TEST(xmod_boxed_primitive_is_int_across_import_2301,
           in_module_control_spec_8_6_28_9) {
    auto dir = wtDir2301();
    // Single-module program. Compiled directly via Driver::compileFile with
    // no deps.
    const std::string src =
        "module WTOneMod2301_t2; "
        "public class Program "
        "  public static method Main(): int "
        "    var n := 5; "
        "    var o: Object := n; "
        "    var inB := (if (o is int) then 1 else 0); "
        "    var k := 7; "
        "    var p: Object := k; "
        "    var backInB := (if (p is int) then 1 else 0); "
        "    Console.WriteLine(\"1|\" + inB.ToString() + \"|\" + backInB.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    std::string stem = unique_stem("bezel_2301t2_");
    auto srcF = std::make_shared<TempFile>((dir / (stem + ".bzl")).string());
    auto exeF = std::make_shared<TempFile>((dir / (stem)).string());
    writeSource(srcF->path, src);
    bezel::Driver driver;
    bezel::CompileResult rb = driver.compileFile(srcF->path, exeF->path, {});
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;
    auto [code, out] = runCapture(exeF->path);
    BEZEL_EXPECT_EQ(code, 0);
    std::string trimmed = out;
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r' || trimmed.back() == ' '))
        trimmed.pop_back();
    BEZEL_EXPECT_EQ(trimmed, std::string("1|1|1"));
}

// ── (T3) PRIMITIVE FAMILY SWEEP — long, bool, double, decimal ──────────────
//
// Same `MakeBox/TakeBox` shape as T1, parameterized over a different boxable
// primitive. The `pvt$<nm>` linkage fix has to apply to EVERY entry in the
// `BZ_BOX_PRIMITIVES` list, not just `Int`. A test per primitive would inflate
// the suite; one test that exercises four representatives pins the broad
// claim. (Pre-fix: each cross-module `is <prim>` returns 0 — the same
// `bz_obj_is_string` discriminator defect regardless of primitive.)
BEZEL_TEST(xmod_boxed_primitive_is_int_across_import_2301,
           primitive_family_sweep_spec_8_6_2_8_3) {
    auto dir = wtDir2301();
    const std::string lib =
        "module WTLib2301_t3; "
        "public class Api "
        "  public constructor() end constructor; "
        "  public static method MkLong(n: long): Object return n; end MkLong; "
        "  public static method TestLong(o: Object): int "
        "    return (if (o is long) then 1 else 0); "
        "  end TestLong; "
        "  public static method MkBool(b: bool): Object return b; end MkBool; "
        "  public static method TestBool(o: Object): int "
        "    return (if (o is bool) then 1 else 0); "
        "  end TestBool; "
        "  public static method MkDbl(d: double): Object return d; end MkDbl; "
        "  public static method TestDbl(o: Object): int "
        "    return (if (o is double) then 1 else 0); "
        "  end TestDbl; "
        "  public static method MkDec(d: decimal): Object return d; end MkDec; "
        "  public static method TestDec(o: Object): int "
        "    return (if (o is decimal) then 1 else 0); "
        "  end TestDec; "
        "  destructor end destructor; "
        "end Api; ";
    const std::string app =
        "module WTApp2301_t3; "
        "import WTLib2301_t3; "
        "public class Program "
        "  public static method Main(): int "
        "    var lo := Api.MkLong(42L); "
        "    var bo := Api.MkBool(true); "
        "    var db := Api.MkDbl(3.5); "
        "    var dc := Api.MkDec(1.25m); "
        "    var slo := Api.TestLong(lo); "
        "    var sbo := Api.TestBool(bo); "
        "    var sdb := Api.TestDbl(db); "
        "    var sdc := Api.TestDec(dc); "
        "    Console.WriteLine(slo.ToString() + \"|\" + sbo.ToString() + \"|\" + sdb.ToString() + \"|\" + sdc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    LibHandle libH = buildLib(dir, "t3", lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, "t3", libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    std::string trimmed = out;
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r' || trimmed.back() == ' '))
        trimmed.pop_back();
    BEZEL_EXPECT_EQ(trimmed, std::string("1|1|1|1"));
}

// (T4 — RECORD FAMILY) — DEFERRED to a follow-up lane. See the SCOPE NOTE
// in the file's axis-grid comment: the §8.6 record-side vtable needs its own
// `boxVtable_[ts]` LinkOnceODR cascade (plus the per-record itmap/itables,
// `boxRecordEqualsThunk`, `boxRecordHashThunk`, `boxToStringThunk`) which is a
// wider blast radius than this one-issue lane is scoped to. The same
// discriminator defect, but a distinct fix surface.

}  // namespace bezel::codegen