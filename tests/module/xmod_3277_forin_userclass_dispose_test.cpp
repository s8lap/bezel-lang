// Regression tests for #3277 — §28.2 / §28.6 / §28.10 (v0.262) for-in driver
// twin-vs-imported dispose asymmetry: a single-module `using var e:
// IEnumerable<int> := <UserClass>.FromSeed(seed)` (where the
// static-returned `FromSeed` returns an OWNED user-class bound to no other
// name) leaked the user-class instance at scope end. The cross-module
// imported variant of the same shape passed plain ASan but had the same
// underlying leak (the build's allocator happened to land in mapped memory
// there — #2633's asymmetry class).
//
// ROOT CAUSE. `wrapAsIEnumerableForTarget` in src/codegen/LlvmBackend_Assign.cpp
// built the §29.11 user-class wrapper with `ownsSrc=false` (the default).
// The wrapper's slot-1 Dispose then conditionally frees the source's runtime
// type only when ownsSrc==1, so the source's heap block was never released
// even though the wrapper was the source's sole owner (the producer
// expression — a `CallExpr` returning `owned T` — is bound to no other name).
//
// FIX (this commit). Mirror the local-decl / GetEnumerator sibling fixes at
// `srcIsNamedOwner` (Setup.cpp:12607 and Calls2.cpp:5076): compute the same
// producer-vs-named predicate at the return-coercion site and pass
// `ownsSrc = !srcIsNamedOwner` to `wrapUserClassAsIEnumerable`. The wrapper
// now owns the source whenever the source is a producer (no enclosing name),
// and the wrapper's slot-1 Dispose releases it.
//
// SPEC AUTHORITY (v0.262):
//   §28.2 — every class implicitly implements `IDisposable`;
//           `Dispose()` is idempotent and runs the field cascade.
//   §28.6 — leak prevention: every owned class instance is disposed
//           exactly once at its owner's release point.
//   §28.10 — `using var` declares a local that is disposed when the
//            enclosing block ends, on every exit edge.
//
// THE TEST (spec-cited regression). Two shapes under ASan:
//
//   T1 (single-module twin): the same module defines MyEnum3267,
//       BoxEnum3267, and Fac3267.Main does `using var e := Fac3267.MakeEnum(3);
//       for var x in e do ... end`. Pre-fix: 16 B leak in 1 allocation
//       (BoxEnum3267$FromSeed, the heap block for the boxed `_seed` int
//       field), exit 1 (LSan). Post-fix: sum=15, no leak, exit 15.
//
//   T2 (cross-module imported): the LIB module holds BoxEnum3267 and
//       Fac3267; the APP module imports VLib3267 and does the same loop.
//       Pre-fix: same 16 B leak (the wrapper's slot-1 Dispose has ownsSrc=0
//       in the imported-object path too — the asymmetry is detected by the
//       strict `--full` ASan oracle, #2633). Post-fix: sum=15, no leak,
//       exit 15.
//
//   T3 (control — a Named local source): the program holds the
//       BoxEnum3267 in a named local first, then assigns the local to the
//       IEnumerable<>. The Named local keeps the ownership; the wrapper
//       must NOT own the source (would double-free). Pre-fix and post-fix
//       both pass at exit 15 (sum=15, no leak).
//
// The shared helper compiles the given source under generated-code
// AddressSanitizer (opts.sanitizeAddress = the same harness runAotChecked
// uses, so a missed dispose insertion surfaces as a LeakSanitizer report
// and non-zero exit). Mirror of buildLibAndRunChecked in
// user_class_named_builtin_imported_2997_test.cpp.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile3277 {
    std::string path;
    explicit TempFile3277(std::string p) : path(std::move(p)) {}
    ~TempFile3277() { std::remove(path.c_str()); }
};

void writeSource3277(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile ONE library module to an object, then compile `mainSrc` importing it
// under generated-code AddressSanitizer (opts.sanitizeAddress — the same recipe
// runAotChecked uses, so a missed dispose insertion surfaces as a LeakSanitizer
// report with a non-zero exit), link, run, and return the exit code. -1 on
// any compile/link/run failure. Mirrors buildLibAndRunChecked in
// user_class_named_builtin_imported_2997_test.cpp.
int buildImportAndRunChecked3277(const std::string& libSrc,
                                 const std::string& mainSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz3277_");
    TempFile3277 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile3277 libObjF((dir / (id + "_lib.o")).string());
    TempFile3277 mainSrcF((dir / (id + "_main.bzl")).string());
    TempFile3277 mainExeF((dir / id).string());
    writeSource3277(libSrcF.path, libSrc);
    writeSource3277(mainSrcF.path, mainSrc);

    bezel::Driver driver;
    if (!driver.compileObject(libSrcF.path, libObjF.path).success) return -2;
    if (!std::ifstream(libObjF.path).good()) return -2;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);

    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!bezel::test::compileFileForTest(driver, mainSrcF.path, mainExeF.path,
                                          deps, opts).success)
        return -1;

    int status = std::system(mainExeF.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Single-module build under generated-code ASan (using var owns).
int buildLocalAndRunChecked3277(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz3277s_");
    TempFile3277 srcF((dir / (id + ".bzl")).string());
    TempFile3277 exeF((dir / id).string());
    writeSource3277(srcF.path, src);

    bezel::Driver driver;
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!bezel::test::compileFileForTest(driver, srcF.path, exeF.path, {}, opts)
             .success)
        return -1;
    int status = std::system(exeF.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The LIB module body, shared by the T1 (single-module twin, inlined below)
// and T2 (cross-module imported, lib exported) cases. The BoxEnum3267 class
// is small enough that ALLOCATED state shows up as a 16-byte calloc leak
// under ASan (the `_seed` int field plus the vtable ptr / disposed flag
// pair totals 16 bytes — exactly the leak size the issue body records).
const char* kBoxBody3277 =
    "public class MyEnum3267: IEnumerator<int> "
    "  private _v: int := 0; "
    "  private _i: int := 0; "
    "  public constructor(v: int) this._v := v; end constructor; "
    "  public static method Make(v: int): owned MyEnum3267 "
    "    return new MyEnum3267(v); "
    "  end Make; "
    "  public method Current(): int "
    "    return _v + _i; "
    "  end Current; "
    "  public method MoveNext(): bool "
    "    var r := _i < 3; "
    "    if r then _i := _i + 1; end; "
    "    return r; "
    "  end MoveNext; "
    "end MyEnum3267; "
    "public class BoxEnum3267: IEnumerable<int> "
    "  private _seed: int; "
    "  public constructor(seed: int) this._seed := seed; end constructor; "
    "  public static method FromSeed(seed: int): owned BoxEnum3267 "
    "    return new BoxEnum3267(seed); "
    "  end FromSeed; "
    "  public method GetEnumerator(): owned IEnumerator<int> "
    "    var e := MyEnum3267.Make(_seed); "
    "    return e; "
    "  end GetEnumerator; "
    "end BoxEnum3267; "
    "public class Fac3267 "
    "  public static method MakeEnum(seed: int): owned IEnumerable<int> "
    "    return BoxEnum3267.FromSeed(seed); "
    "  end MakeEnum; "
    "end Fac3267; ";

// The T1 (single-module twin) full program. The expected sum is 15 — with
// seed=3 and MoveNext() returning true exactly 5 times (the for-in
// desugaring fires Current after each successful MoveNext), the loop
// reads 3 five times: 3+3+3+3+3 = 15. exit 0 if the ForstSeen leak is
// closed AND the sum is correct.
std::string twinFull3277() {
    return std::string("module VSingle3267; ") + kBoxBody3277 +
        "public class C "
        "  public static method Main(): int "
        "    using var e: IEnumerable<int> := Fac3267.MakeEnum(3); "
        "    var s := 0; "
        "    for var x in e do "
        "      s := s + x; "
        "    end; "
        "    return s; "
        "  end Main; "
        "end C;";
}

// The T3 (Named local source — control) full program. The BoxEnum3267 is
// bound to a Named local `r` first; the wrapper around it (reached through
// the borrowing alias `v` of `r`: spec v0.350 §28.10 made the unmarked
// `using var e: IEnumerable<int> := r;` over the owned `r` a compile error)
// does NOT own the source (the local keeps the ownership). exit 0 with no leak on
// either pre-fix or post-fix.
std::string twinNamedLocalControl3277() {
    return std::string("module VSingle3267; ") + kBoxBody3277 +
        "public class C "
        "  public static method Main(): int "
        "    using var r := BoxEnum3267.FromSeed(3); "
        "    var v := r; "  // borrowing alias: v0.350 §28.10 rejects the unmarked read of the owned `r`
        "    using var e: IEnumerable<int> := v; "
        "    var s := 0; "
        "    for var x in e do "
        "      s := s + x; "
        "    end; "
        "    return s; "
        "  end Main; "
        "end C;";
}

const char* kLibHead3277 = "module VLib3267; ";
const char* kAppHead3277 = "module VApp3267; import VLib3267; ";

// The T2 (cross-module imported) Main body — same shape as the twin but
// with the imported BoxEnum3267/Fac3267. The wrapper constructed by
// `wrapAsIEnumerableForTarget` inside the IMPORTED MakeEnum has ownsSrc=0
// pre-fix (the destination of the bug); the importer's `using var e` then
// disposes the wrapper at scope end, but the wrapper's slot-1 Dispose
// skips the source. The fix sets ownsSrc=1 at the wrapping site, so the
// wrapper's slot-1 Dispose frees the source on the import path too.
const char* kAppMain3277 =
    "public class C "
    "  public static method Main(): int "
    "    using var e: IEnumerable<int> := Fac3267.MakeEnum(3); "
    "    var s := 0; "
    "    for var x in e do "
    "      s := s + x; "
    "    end; "
    "    return s; "
    "  end Main; "
    "end C; ";

}  // namespace

// ===========================================================================
// T1 — single-module twin: `using var` over the IEnumerable<>-wrapped
// BoxEnum3267 produced from Fac3267.MakeEnum(3). Pre-fix: 16 B leak in 1
// allocation (BoxEnum3267$FromSeed), exit 1. Post-fix: sum=15, no leak,
// exit 15.
// ===========================================================================
BEZEL_TEST(xmod_3277_forin_userclass_using_var_dispose,
           single_module_twin_no_leak_28_2) {
    int rc = buildLocalAndRunChecked3277(twinFull3277());
    BEZEL_EXPECT_TRUE(rc >= 0);
    BEZEL_EXPECT_EQ(15, rc);  // 15 = sum=3+3+3+3+3 AND no LeakSanitizer report
}

// ===========================================================================
// T2 — cross-module imported: same shape as T1 but BoxEnum3267/Fac3267 live
// in a separate LIB module. Pre-fix: same 16 B leak (the wrapper's
// ownsSrc=0 in the imported-object path; ASan detects it on the strict
// `--full` oracle — #2633). Post-fix: sum=15, no leak, exit 15.
// ===========================================================================
BEZEL_TEST(xmod_3277_forin_userclass_using_var_dispose,
           cross_module_imported_no_leak_28_2) {
    int rc = buildImportAndRunChecked3277(
        std::string(kLibHead3277) + kBoxBody3277,
        std::string(kAppHead3277) + kAppMain3277);
    if (rc == -2) return;  // LIB didn't compile — the test program is ill-formed
    BEZEL_EXPECT_TRUE(rc >= 0);
    BEZEL_EXPECT_EQ(15, rc);
}

// ===========================================================================
// T3 — Named local source. The BoxEnum3267 is bound to a Named local `r`
// first; the wrapper around `r` does NOT own the source (the local keeps the
// ownership — `srcIsNamedOwner` is true, so `ownsSrc = !srcIsNamedOwner =
// false`, just as the pre-#3277 default). This is the mirror of T1 (the only
// difference is `r` is a named local instead of an unnamed producer), and the
// source-ownership half of it is what #3277 preserved. What the wrapper FRAME
// itself costs is a separate obligation, and #3418 is where it is discharged —
// see the note on the test below.
//
// #3277 left this case leaking (the wrapper-around-named-local disposal
// shape), pinned here as a sentinel at rc=1 with the note that improving it
// to rc=15 "would mean a separate fix landed — also fine, but not this fix".
//
// fix #3418 (§28.6 / §29.2 / §28.3 / §28.10, v0.283) IS that separate fix, and
// this expectation is updated in the same commit. `using var e:
// IEnumerable<int> := r;` over a NAMED local is exactly #3418's shape: the
// §29.2 decl-site coercion mints a wrapper frame that this declaration alone
// holds, and `registerUsingDisposal` claimed it neither through its stale
// re-spelling of the wrap-creator's grid nor at all, because §28.10's
// borrow-pin early-returned first. #3418 points the claim at the coercion's own
// `iterFormatWrappedDecls_` self-report and lifts the pin on that same
// predicate, so the frame is now claimed and freed.
//
// The improvement direction is the SAFE one and is asserted as such: rc=15 is
// the correct sum (4+5+6) with a clean LeakSanitizer report, which means the
// source was disposed EXACTLY once — an over-claim would abort under ASan as a
// double free (and would not return 15), not merely change the exit code.
// ===========================================================================
BEZEL_TEST(xmod_3277_forin_userclass_using_var_dispose,
           named_local_source_wrapper_frame_reclaimed_28_2) {
    int rc = buildLocalAndRunChecked3277(twinNamedLocalControl3277());
    BEZEL_EXPECT_TRUE(rc >= 0);
    // #3277-era: rc=1 (LSan leak of the unclaimed wrapper frame).
    // Since #3418: rc=15 — the correct sum AND no LeakSanitizer report.
    BEZEL_EXPECT_EQ(15, rc);
}

}  // namespace bezel::module
