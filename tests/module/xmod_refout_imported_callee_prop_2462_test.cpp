// fix #2462 (§22.3, §2.8.2 clause 1 / §2.8.3 rule 4; v0.235): calling an
// IMPORTED method with `ref`/`out` pointing at a property (or indexer)
// target silently dropped the write — no diagnostic, wrong runtime value.
//
// §22.3 "`ref` and `out` argument locations" (v0.235): a `ref`/`out`
// argument's location is "an indexer slot, or a property with an accessible
// setter" — "such a target is instead copy-in / copy-out": the receiver and
// index expressions are evaluated once, the getter (for `ref`) seeds the
// callee's slot, and on normal return the setter is called once with the
// slot's final value.
//
// FOUND VIA #2454's OWN VERIFY-FIRST: two call-argument-building code paths
// in `LlvmBackend_Calls2.cpp` — the plain `.` method-call form (the `else`
// branch on `dynamic_cast<const ast::MethodDecl*>(ms->decl)` being null,
// i.e. an IMPORTED callee, §22.3/#1221's own comment) and the `?.`
// null-conditional form's identical twin — built `ref`/`out` arguments with
// a bare `lvalueAddr(*a.value, et)` call and NOTHING ELSE. `lvalueAddr`
// resolves only TRUE-ALIAS locations (a local, a field, a static, an
// inline-array element, §22.3: "a `ref` argument naming a mutable local or
// writable field is a true alias"); a property or indexer has no address to
// alias and needs the copy-in/copy-out bracket
// (`emitAccessorRefArgCopyIn`/`emitAccessorRefArgWriteBacks`, fix #1302) —
// which neither of these two branches ever fell back to. `lvalueAddr`
// returning null then just abandoned the ENTIRE containing call (`if (!v)
// return nullptr;`) with NO diagnostic anywhere — the statement compiled
// clean and, at runtime, the callee's body never even ran (the receiver's
// `d.Name` was never read OR written).
//
// CONFIRMED (verify-first, before any fix): the exact repro below compiled
// clean (`bezelc --import lib.o app.bzl` exit 0) and ran (exit 0), but
// `d.Name` after the call was still its untouched initial value 1, not the
// expected 2 — proving the write never happened (not merely "no crash").
// The LOCAL-callee equivalent of the identical property target, by
// contrast, already worked correctly once #2454's own codegen half
// (`lowerImportedPropertySetValue` wired into
// `emitAccessorRefArgWriteBacks`) landed — proving the property-target
// machinery itself was sound; the gap was specifically in how arguments are
// built for a call whose CALLEE is imported.
//
// FIX: both branches now fall back to `emitAccessorRefArgCopyIn` (restricted
// to plain `ref`/`out`, mirroring `buildOrderedArgs`'s own restriction —
// `out owned` etc. are NOT admitted into the bracket per §22.3) when
// `lvalueAddr` fails, populating the SAME `accessorRefArgs` sink each
// enclosing function already flushes unconditionally via
// `emitAccessorRefArgWriteBacks` after the call — reusing existing
// machinery, no new mechanism. `emitAccessorRefArgCopyIn`'s signature
// changed from taking `const ast::Param&` to a pre-lowered `llvm::Type*
// paramTy` (nullable), because neither new call site has an `ast::Param` at
// all (an imported method's AST is never reconstructed, #1221's existing
// limitation) — `paramTy` only refines slot WIDENING (the #2266 slot-clobber
// shape); §22.3's "exact referent type" rule means the ordinary case has
// `paramTy == valueTy` regardless, so a null `paramTy` does not affect
// correctness for a well-typed program, only the narrow widening refinement.
//
// SCOPE: verified BEFORE landing this fix that #2454's own codegen change
// (from the SAME investigation trail) does NOT already fix this — it only
// wired the property-target write-back for a LOCAL callee; this fix's own
// probe on current `main` (post-#2454) still showed the silent drop, so
// this is not a stale-duplicate report.
//
// Mechanically verified fail-without/pass-with: saved the src/ fix as a
// scratch-copied patch, reverted via `git checkout HEAD --` (NOT `git
// stash` — shared worktree refs/stash), rebuilt, confirmed T1/T2/T3/T6
// compiled clean but returned the WRONG value (the write silently
// dropped), while T4 (reject-control) and T5 (local-variable control) were
// unaffected either way; reapplied the byte-identical patch via `git
// apply`, rebuilt, confirmed all pass with the asserted values.

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

struct TempFile2462 {
    std::string path;
    explicit TempFile2462(std::string p) : path(std::move(p)) {}
    ~TempFile2462() { std::remove(path.c_str()); }
};

void writeSource2462(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2462(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B (importing A's compiled object), capturing the driver's stderr
// (fd 2) into `err`. Mirrors xmod_refout_prop_visibility_2454_test.cpp's
// compileImportCaptureErr2454.
bool compileImportCaptureErr2462(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath = (fs::temp_directory_path() /
                           ("bz2462_err_" + unique_stem("") +
                            ".txt"))
                              .string();

    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj;
        d.objectPath = aObj;
        deps.push_back(d);
        success = driver.compileFile(bSrc, bExe, deps).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);

    std::ifstream in(errPath);
    err.assign((std::istreambuf_iterator<char>(in)),
               std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return success;
}

// Compile B importing A -> exe, run in a SEPARATE process (fork-isolated per
// tests/README.md), return exit code (-1 on compile/link/run failure, -2 if
// the dependency itself could not be produced).
int compileImportAndRun2462(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2462_");
    TempFile2462 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2462 aObj((dir / (stem + "_a.o")).string());
    writeSource2462(aSrc.path, libSrc);
    if (!compileToObj2462(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2462 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2462 bExe((dir / stem).string());
    writeSource2462(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!driver.compileFile(bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — the exact #2462 repro: `ref` argument to an IMPORTED method
// (`Bump`, declared on the imported class), targeting an imported PUBLIC
// settable property. Pre-fix: compiled clean, ran, but the write silently
// never happened (Name stayed 1, not 2). Asserts the round-trip value.
// ===========================================================================
BEZEL_TEST(xmod_refout_imported_callee_prop_2462,
           ref_to_imported_method_on_imported_property_writes_correctly) {
    int r = compileImportAndRun2462(
        "module RefOutProbeLib2462T1; "
        "public class RefOutProbeClass2462T1 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    public set => _n := value; "
        "  end Name; "
        "  public method Bump(ref v: int) "
        "    v := v + 1; "
        "  end Bump; "
        "end RefOutProbeClass2462T1;",
        "module RefOutProbeApp2462T1; import RefOutProbeLib2462T1; "
        "public class C "
        "  public static method Main(): int "
        "    using var d: RefOutProbeClass2462T1 := new "
        "RefOutProbeClass2462T1(); "
        "    d.Bump(ref d.Name); "
        "    return d.Name - 2; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T2 — the `out` twin of T1: an IMPORTED method's `out` parameter targeting
// a WRITE-ONLY imported property (no getter — §22.3: "for `out` no getter
// call occurs", so this is a legal `out` target even though it would not be
// a legal `ref` one). Reads back through a getter METHOD (`Peek`, since the
// property itself has no getter) to observe the value actually written.
// ===========================================================================
BEZEL_TEST(xmod_refout_imported_callee_prop_2462,
           out_to_imported_method_on_write_only_property_writes_correctly) {
    int r = compileImportAndRun2462(
        "module WriteOnlyOutLib2462T2; "
        "public class WriteOnlyOutClass2462T2 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    set => _n := value; "
        "  end Name; "
        "  public method Peek(): int => _n; "
        "  public method Produce(out v: int) "
        "    v := 42; "
        "  end Produce; "
        "end WriteOnlyOutClass2462T2;",
        "module WriteOnlyOutApp2462T2; import WriteOnlyOutLib2462T2; "
        "public class C "
        "  public static method Main(): int "
        "    using var d: WriteOnlyOutClass2462T2 := new "
        "WriteOnlyOutClass2462T2(); "
        "    d.Produce(out d.Name); "
        "    return d.Peek() - 42; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T3 — the `?.` null-conditional call form's identical twin gap (the SAME
// "no accessor bracket on this path" limitation the source comment used to
// describe, in the SEPARATE null-conditional-call code path). `ref` to an
// IMPORTED method reached through `d?.Bump(...)`, targeting an imported
// public property.
// ===========================================================================
BEZEL_TEST(xmod_refout_imported_callee_prop_2462,
           ref_to_imported_method_via_null_conditional_call_writes_correctly) {
    int r = compileImportAndRun2462(
        "module NullCondLib2462T3; "
        "public class NullCondClass2462T3 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    public set => _n := value; "
        "  end Name; "
        "  public method Bump(ref v: int) "
        "    v := v + 1; "
        "  end Bump; "
        "end NullCondClass2462T3;",
        "module NullCondApp2462T3; import NullCondLib2462T3; "
        "public class C "
        "  public static method Main(): int "
        "    using var d: NullCondClass2462T3? := new "
        "NullCondClass2462T3(); "
        "    d?.Bump(ref d.Name); "
        "    return d.Name - 2; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T4 — REJECT-CONTROL: a `private set` imported property as a `ref`
// argument to an IMPORTED method is still, correctly, rejected — #2454's
// sema check (`checkRefOutPropertyOperand`) runs regardless of whether the
// CALLEE is local or imported (it gates on the property, not the callee),
// so this fix (a purely codegen-side change) must not affect it.
// ===========================================================================
BEZEL_TEST(xmod_refout_imported_callee_prop_2462,
           private_set_property_to_imported_method_still_rejected_control) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2462_t4_");
    TempFile2462 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2462 aObj((dir / (stem + "_a.o")).string());
    writeSource2462(aSrc.path,
                    "module PrivSetLib2462T4; "
                    "public class PrivSetClass2462T4 "
                    "  private _n: int := 1; "
                    "  public property Name: int "
                    "    get => _n; "
                    "    private set => _n := value; "
                    "  end Name; "
                    "  public method Bump(ref v: int) "
                    "    v := v + 1; "
                    "  end Bump; "
                    "end PrivSetClass2462T4;");
    BEZEL_EXPECT_TRUE(compileToObj2462(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;  // toolchain unavailable

    std::string bSrc =
        "module App2462T4; import PrivSetLib2462T4; "
        "public class C "
        "  public static method Main(): int "
        "    using var d: PrivSetClass2462T4 := new PrivSetClass2462T4(); "
        "    d.Bump(ref d.Name); "
        "    return 0; "
        "  end Main; "
        "end C;";
    TempFile2462 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2462 bExe((dir / (stem + "_b")).string());
    writeSource2462(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2462(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("`ref` argument") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("not accessible") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("13.6") != std::string::npos);
}

// ===========================================================================
// T5 — CONTROL: a plain LOCAL VARIABLE `ref` argument to an IMPORTED
// method still works (the TRUE-ALIAS `lvalueAddr` path, which this fix
// does not touch — the fallback only fires when `lvalueAddr` fails).
// Guards against the fix breaking the already-working common case.
// ===========================================================================
BEZEL_TEST(xmod_refout_imported_callee_prop_2462,
           local_variable_ref_to_imported_method_still_works_control) {
    int r = compileImportAndRun2462(
        "module LocalVarLib2462T5; "
        "public class LocalVarClass2462T5 "
        "  public method Bump(ref v: int) "
        "    v := v + 1; "
        "  end Bump; "
        "end LocalVarClass2462T5;",
        "module LocalVarApp2462T5; import LocalVarLib2462T5; "
        "public class C "
        "  public static method Main(): int "
        "    using var h: LocalVarClass2462T5 := new LocalVarClass2462T5(); "
        "    var x: int := 1; "
        "    h.Bump(ref x); "
        "    return x - 2; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T6 — the SAME imported-callee gap, but for a LOCAL (same-module) property
// target instead of an imported one — proves the fix addresses the CALLEE
// side of the gap generally, not just the specific imported-property shape
// #2462 was filed against.
// ===========================================================================
BEZEL_TEST(xmod_refout_imported_callee_prop_2462,
           ref_to_imported_method_on_local_property_writes_correctly) {
    int r = compileImportAndRun2462(
        "module ImportedCalleeLib2462T6; "
        "public class ImportedCalleeClass2462T6 "
        "  public method Bump(ref v: int) "
        "    v := v + 1; "
        "  end Bump; "
        "end ImportedCalleeClass2462T6;",
        "module ImportedCalleeApp2462T6; import ImportedCalleeLib2462T6; "
        "public class LocalPropHolder2462T6 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end LocalPropHolder2462T6; "
        "public class C "
        "  public static method Main(): int "
        "    using var h: ImportedCalleeClass2462T6 := new "
        "ImportedCalleeClass2462T6(); "
        "    using var p: LocalPropHolder2462T6 := new "
        "LocalPropHolder2462T6(); "
        "    h.Bump(ref p.Name); "
        "    return p.Name - 2; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

}  // namespace bezel::module
