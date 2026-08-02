// fix #2485 (§9.2/§22.8/§29.2/§29.11, v0.235): the DISPOSAL/ARC subset of the
// ~30 remaining `sema::isAutoPropertySymbol` / `Emitter::isAutoProperty` call
// sites left decl-dependent after #2478's storage-layout fix.
//
// #2478 added `MetaMember::isAutoProperty` (a wire-carried classification for
// an imported Property, published via `SemaContext::importedPropertyIsAutoStorage`)
// and `Emitter::isAutoPropertyStorage` (local decl-based check first, wire-
// carried fallback for a decl-null imported Property) — but scoped its use to
// exactly the four storage-LAYOUT call sites responsible for the confirmed
// OOB-write hazard #2478 fixed. This issue's own audit found ~30 further call
// sites of the bare (still decl-dependent) predicates, grouped by presumed
// failure direction. This test covers family A — the for-in "named vs.
// computed" disposal classification, seven near-identical copies across
// LlvmBackend_Stmts.cpp / LlvmBackend_Calls2.cpp / LlvmBackend_Setup.cpp.
//
// ROOT CAUSE: per §29.2/§29.11, a for-in loop DISPOSES its iterable SOURCE
// only when the source is a freshly produced, unbound temporary (a CallExpr/
// NewExpr result with no owning binding) — a NAMED source (a local, param,
// field, OR AUTO-PROPERTY) is a stable binding that the loop merely BORROWS;
// disposing it would free something still reachable through that binding.
// The classification test — `sema::isAutoPropertySymbol(ps)` /
// `Emitter::isAutoProperty(ps)` — requires `dynamic_cast<const
// ast::PropertyDecl*>(ps->decl)`, which is unconditionally null for an
// IMPORTED PropertySymbol (ModuleLoader.cpp never reconstructs one). So an
// imported auto-property for-in source was always misclassified as
// "computed" (the fresh-temporary case) — the loop then disposed a source it
// does not own.
//
// CONFIRMED LIVE MEMORY-SAFETY HAZARD (independently reproduced for this
// issue, per the coordinator's explicit instruction NOT to build on #2478's
// own inferred-but-untested severity ranking): a library class exposes a
// public auto-property `Items: IEnumerable<int>` initialized from a yield
// method's result. An importing module enumerates `h.Items` in a for-in loop
// TWICE. Pre-fix: the FIRST loop's erroneous disposal frees the source's
// iterator-frame block; the SECOND loop re-reads `h.Items` (the same,
// now-dangling pointer — the auto-property's backing field is never nulled by
// the loop's disposal) and calls `GetEnumerator()` through it — a heap-use-
// after-free, confirmed under AddressSanitizer (`bz$iter$Seq$Dispose` frees
// the block `bz$Lib.Gen$Seq` allocated; the second loop's `GetEnumerator`
// vtable read then faults inside the freed block). This matches the issue's
// predicted direction exactly ("fails toward disposing a BORROWED source").
//
// THE FIX: `isAutoPropertyStorage` (already built by #2478, unchanged here)
// replaces the bare predicate at the three near-identical copies in
// LlvmBackend_Stmts.cpp (`lowerIEnumerableBuiltinFor`'s sync-IEnumerable path,
// its self-returning-iterator idiom path, and `lowerAwaitFor`'s async twin),
// the two in LlvmBackend_Calls2.cpp (an explicit `.GetEnumerator()` call's
// receiver classification, both the wrapUserClassAsIEnumerable site and its
// #2376 sibling), and the two in LlvmBackend_Setup.cpp (the `IEnumerable<T>`-
// typed local-declaration coercion's `wrapUserClassAsIEnumerable` call, and
// the manual-protocol `.GetAsyncEnumerator()` self-alias guard). Every site is
// the SAME "is this for-in/enumerator source a named (borrowed) auto-property
// vs. a fresh (loop-owned) computed value" question #2478's own commit
// enumerated as its own follow-up scope.
//
// SCOPE (CLAUDE.md rule 4 — deliberately narrow, matching #2478's own
// discipline): only the disposal/ARC ("family A") for-in-classification sites
// are touched here. Two OTHER call-site families this issue's own audit
// found — sema spurious-rejects (family B, DefiniteAssignment.cpp/
// ExprTypecheck_Exprs.cpp) and serialization-eligibility silent-wrong-data
// (family C, TypeResolution_Checks.cpp/SymbolCollection.cpp) — are explicitly
// OUT OF SCOPE for this change and remain filed on #2485 for a dedicated
// follow-up. A FOURTH, lower-confidence sub-family this issue flagged (ARC
// retain/release for `importedObj.AutoProp := v`, i.e. `isStringFieldLvalue`/
// `isClosureFieldLvalue`/`isBoxFieldLvalue` in LlvmBackend_Setup.cpp) was
// investigated and found to be UNREACHABLE with an imported PropertySymbol
// under the current architecture: every cross-module property-SET this
// investigation could construct is intercepted earlier by
// `Emitter::lowerImportedPropertySet` (fix #2400's explicit `$propset`-call
// fallback for a MemberAccessExpr target) before ever reaching those three
// predicates, or a plain bare-identifier `Name := v;` write inside a LOCAL
// class inheriting an IMPORTED base's auto-property landed on a totally
// unguarded plain-store fallback with a DIFFERENT (unconfirmed) root cause —
// so a mechanical fail-red/pass-green test for those three predicates could
// not be produced; NOT touched here (see issue #2485's own follow-up comment
// for the investigation notes, and #2490 for the newly-discovered, separate
// bug this traced to instead — an auto-property SETTER body with no ARC
// protocol at all for a string/closure/box-typed field).
//
// Fail-red / pass-green: reverting the three isAutoPropertyStorage call sites
// this test exercises (Stmts.cpp's two sync-for-in copies) back to
// `sema::isAutoPropertySymbol` reproduces the heap-use-after-free T1 pins
// (confirmed by hand before authoring this test, matching the lane's
// "reproduce, then fix" discipline).
//
// Observability: a custom Driver::compileFile(..., opts) call with
// `opts.sanitizeAddress = true`, linked and run with
// `ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:exitcode=97` — leak detection
// is deliberately OFF because the auto-property's backing field is a PLAIN
// (non-`owned`) storage location (§28.5: only a field/property explicitly
// marked `owned`, or captured by an `owned` constructor parameter, is
// auto-disposed at the container's own disposal) — nothing in this program
// ever disposes the source IEnumerable, so it leaks by construction on BOTH
// the buggy and the fixed compiler; that pre-existing, orthogonal gap is not
// part of this issue and is not what this test pins. `exitcode=97` gives the
// crash path (heap-use-after-free) a distinct, unambiguous exit code from any
// legitimate program result.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile2485 {
    std::string path;
    explicit TempFile2485(std::string p) : path(std::move(p)) {}
    ~TempFile2485() { std::remove(path.c_str()); }
};

void writeSource2485(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2485(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compiles `appSrc` importing `libObj`, links + runs under ASan with leak
// detection off (see the file header for why) and a distinct crash exit code.
// Returns:
//   the program's exit code on a clean run,
//   97 if AddressSanitizer caught a memory-safety error,
//   -1 on a compile/link failure,
//   -2 if the toolchain is unavailable (SKIP, not a failure).
int compileImportRunAsan2485(const std::string& libSrc,
                             const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2485_" + std::to_string(std::rand());
    TempFile2485 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2485 aObj((dir / (stem + "_a.o")).string());
    writeSource2485(aSrc.path, libSrc);
    if (!compileToObj2485(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2485 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2485 bExe((dir / stem).string());
    writeSource2485(bSrc.path, appSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!driver.compileFile(bSrc.path, bExe.path, deps, opts).success)
        return -1;

    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:exitcode=97 " +
        bExe.path + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(bExe.path.c_str());
    if (status == -1) return -2;  // system() itself failed — SKIP
    if (WIFSIGNALED(status)) return 97;  // ASan aborted before exitcode ran
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// Mirrors compileImportRunAsan2485 but for a SINGLE-module (no import)
// program — the local-only control, unaffected by decl-dependence.
int compileRunAsan2485Local(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2485_local_" + std::to_string(std::rand());
    TempFile2485 srcFile((dir / (stem + ".bzl")).string());
    TempFile2485 exe((dir / stem).string());
    writeSource2485(srcFile.path, src);

    bezel::Driver driver;
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!driver.compileFile(srcFile.path, exe.path, {}, opts).success)
        return -1;

    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:exitcode=97 " +
        exe.path + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(exe.path.c_str());
    if (status == -1) return -2;
    if (WIFSIGNALED(status)) return 97;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

const char* kLib2485 =
    "module Lib2485; "
    "public class Gen2485 public static method Seq(): IEnumerable<int> "
    "  yield return 10; yield return 20; yield return 30; "
    "end Seq; end Gen2485; "
    "public class Holder2485 "
    "  public property Items: IEnumerable<int> := Gen2485.Seq(); "
    "end Holder2485;";

}  // namespace

// ===========================================================================
// T1 — CORE repro: a for-in loop over an IMPORTED auto-property source, run
// TWICE. Pre-fix the first loop's erroneous disposal frees the shared source;
// the second loop's re-read of `h.Items` (the same dangling pointer) then
// dereferences freed memory — a heap-use-after-free (independently confirmed
// under ASan before this fix landed, per the coordinator's explicit
// instruction to test the inferred risk rather than build on it). Post-fix,
// per §29.2, the auto-property source is BORROWED and never disposed by
// either loop — 60+60 = 120, ASan-clean (no crash; the source itself leaks by
// construction — see file header — hence detect_leaks=0).
// ===========================================================================
BEZEL_TEST(xmod_auto_property_forin_disposal_2485,
           double_forin_over_imported_auto_property_source_no_uaf_29_2) {
    const std::string appSrc =
        "module App2485T1; import Lib2485; "
        "public class Program2485T1 "
        "  public static method Main(): int "
        "    using var h := new Holder2485(); "
        "    var total: int := 0; "
        "    for var x in h.Items do total := total + x; end for; "
        "    for var x in h.Items do total := total + x; end for; "
        "    return total; "
        "  end Main; "
        "end Program2485T1;";
    int r = compileImportRunAsan2485(kLib2485, appSrc);
    if (r == -2) return;  // ASan toolchain / dependency build unavailable — skip
    BEZEL_EXPECT_EQ(r, 120);  // (10+20+30) * 2, no crash
}

// ===========================================================================
// T2 — CONTROL: the IDENTICAL shape, but fully LOCAL (no import) — decl != null
// for every symbol, so `isAutoPropertyStorage` takes its first (unchanged,
// pre-existing `isAutoProperty`) branch exactly as before this fix. Guards
// against a regression in the already-correct local classification.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_forin_disposal_2485,
           double_forin_over_local_auto_property_source_control_29_2) {
    const std::string src =
        "module LocalCtl2485T2; "
        "public class Gen2485T2 public static method Seq(): IEnumerable<int> "
        "  yield return 10; yield return 20; yield return 30; "
        "end Seq; end Gen2485T2; "
        "public class Holder2485T2 "
        "  public property Items: IEnumerable<int> := Gen2485T2.Seq(); "
        "end Holder2485T2; "
        "public class Program2485T2 "
        "  public static method Main(): int "
        "    using var h := new Holder2485T2(); "
        "    var total: int := 0; "
        "    for var x in h.Items do total := total + x; end for; "
        "    for var x in h.Items do total := total + x; end for; "
        "    return total; "
        "  end Main; "
        "end Program2485T2;";
    int r = compileRunAsan2485Local(src);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 120);
}

// ===========================================================================
// T3 — CONTROL: a for-in over a freshly produced, UNBOUND temporary source
// (`for var x in Gen2485.Seq() do ... end`, a CallExpr with no owning
// binding), imported. Per §29.11 this IS loop-owned and must still be
// disposed once at loop end — the fix must not regress the temporary-source
// leak-prevention path into disposing nothing. Single enumeration only (a
// fresh temporary is exhausted after one pass); ASan-clean, sum 60.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_forin_disposal_2485,
           imported_temporary_source_still_enumerates_control_29_11) {
    const std::string appSrc =
        "module App2485T3; import Lib2485; "
        "public class Program2485T3 "
        "  public static method Main(): int "
        "    var total: int := 0; "
        "    for var x in Gen2485.Seq() do total := total + x; end for; "
        "    return total; "
        "  end Main; "
        "end Program2485T3;";
    int r = compileImportRunAsan2485(kLib2485, appSrc);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 60);
}

}  // namespace bezel::module
