// fix #2454 (§22.3, §13.6, §23.3, §2.8.2 clause 1 / §2.8.3 rule 4; v0.235):
// `checkRefOutPropertyOperand` (ExprTypecheck.cpp) skipped BOTH the "no
// setter" check and the §13.6 setter-visibility check for an IMPORTED
// property used as a `ref`/`out` argument — the ref/out sibling of #2412
// (hasSetter) and #2415 (setter visibility), which fixed the identical gap
// for the plain `:=` assignment-target check in the same file.
//
// §22.3 "`ref` and `out` argument locations" (v0.235): a `ref`/`out`
// argument must be an assignable storage location — "...a property with an
// accessible setter". Both `ref` and `out` require the setter (the copy-out
// half calls it unconditionally on normal return); only `ref` ALSO requires
// a getter ("for `ref`, the getter is called once to produce the callee's
// slot value; for `out` no getter call occurs") — so a write-only property
// is a legal `out` target but not a legal `ref` target. That getter-side
// distinction is NOT enforced by this fix (see the SCOPE note below); the
// hasSetter / setter-visibility gap this fix closes applies identically to
// both modifiers.
//
// FOUND VIA THIS FIX'S OWN VERIFY-FIRST: `checkRefOutPropertyOperand` had
// `auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps.decl); if (!pd)
// return;` — unconditionally true for ANY imported property (ModuleLoader
// never reconstructs a body-less imported property's decl) — so BOTH a
// `private set` AND a getter-only imported property compiled clean as a
// `ref` argument (confirmed by probe before this fix, filed as #2454).
//
// FIX: mirrors #2412/#2415 exactly — an `else` branch on `!pd`, falling
// back to the SAME two side tables (`ctx_.importedPropertyHasSetter` /
// `ctx_.importedPropertySetterVisibility`), applying the identical §13.1
// access rules already written for the `:=` site. No new metadata needed.
//
// A SECOND, INDEPENDENT gap surfaced during verify-first: even once sema
// correctly ACCEPTS a legitimate case (e.g. `public set`), the copy-out
// write-back (`emitAccessorRefArgWriteBacks`, LlvmBackend_Assign.cpp) had no
// fallback for a PLAIN (non-virtual, non-interface) imported property —
// `lowerVirtualPropertySet` needs a vtable/itable slot this shape has none
// of; `lowerPropertySetValue` needs an `ast::PropertyDecl`, which an
// imported PropertySymbol never has — so the write-back hit its own
// "internal: no setter ... — #1302" diagnostic even for a fully accessible
// imported property (confirmed by probe: a LOCAL method taking `ref v: int`
// called as `Bump(ref imported.PublicSetProp)` failed this way pre-fix).
// Fixed by adding `lowerImportedPropertySetValue` (#2400) as a third
// fallback — reusing the SAME importedPropSetterFns_-backed function #2400
// already wired for the DIRECT (non-bracket) `:=` case; no new machinery.
//
// SCOPE (recorded per CLAUDE.md rule 4, not silently applied):
//   - A THIRD, independent gap was found and NOT fixed here: calling an
//     IMPORTED METHOD (not a local one) with `ref`/`out` pointing at a
//     property target silently drops the write entirely (no diagnostic,
//     no error, wrong runtime value) — `emitAccessorRefArgCopyIn` is never
//     even invoked for that call shape, a different code path from the one
//     this fix touches. Filed as #2462 (higher severity: a SILENT
//     correctness bug, not a compile-time rejection).
//   - The `ref`-needs-a-getter / `out`-does-not distinction (§22.3) is not
//     enforced for an imported property missing a getter — recovering
//     "does this imported property have a getter" would need a NEW,
//     currently-nonexistent metadata bit (no `hasGetter`/`isSetOnly` scalar
//     exists in `MetaMember` today), so this fix does not guess at one.
//     Today a write-only imported property used as `ref` is still soundly
//     REJECTED — just via the same pre-existing "internal: no setter —
//     #1302" codegen diagnostic (since the copy-in getter call has nothing
//     to call), not a clean §23 message. Filed as #2461, a message-
//     quality follow-up (not a soundness gap: it already rejects).
//   - While isolating T8 below (protected-set access from a local subclass
//     of an IMPORTED base), an UNRELATED, pre-existing bug surfaced: a
//     local class extending an imported base class does not run the
//     imported base's field initializers at all (confirmed with a PLAIN
//     PUBLIC FIELD, no property/ref/out involved: `class D: ImportedBase
//     end D;` then `new D()._n` reads 0, not the base's declared initial
//     value 1). T8 is written with a DELTA assertion (`after - before ==
//     1`) specifically to isolate THIS fix's correctness from that
//     unrelated bug (which affects the *initial* value, not the delta a
//     `ref` write applies). Filed as #2463, its own high-severity issue.
//
// Mechanically verified fail-without/pass-with for both halves of this fix
// (sema + codegen): saved the src/ fix as a scratch-copied patch, reverted
// via `git checkout HEAD --` (NOT `git stash` — shared worktree
// refs/stash), rebuilt, confirmed T1/T2/T3/T4/T7/T9/T10 compiled CLEAN (the
// sema bug) and T5/T6/T8 hit the codegen "internal: no setter — #1302"
// diagnostic (the codegen bug, reached only once the sema half admits the
// accessible cases) rather than running to a correct value; reapplied the
// byte-identical patch via `git apply`, rebuilt, confirmed all pass with
// the asserted diagnostics/values.

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

struct TempFile2454 {
    std::string path;
    explicit TempFile2454(std::string p) : path(std::move(p)) {}
    ~TempFile2454() { std::remove(path.c_str()); }
};

void writeSource2454(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2454(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B (importing A's compiled object), capturing the driver's stderr
// (fd 2) into `err`. Mirrors xmod_prop_setter_visibility_2415_test.cpp's
// compileImportCaptureErr2415.
bool compileImportCaptureErr2454(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath = (fs::temp_directory_path() /
                           ("bz2454_err_" + std::to_string(std::rand()) +
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
int compileImportAndRun2454(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2454_" + std::to_string(std::rand());
    TempFile2454 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2454 aObj((dir / (stem + "_a.o")).string());
    writeSource2454(aSrc.path, libSrc);
    if (!compileToObj2454(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2454 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2454 bExe((dir / stem).string());
    writeSource2454(bSrc.path, appSrc);
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
// T1 — `private set`, IMPORTED, `ref` argument, LOCAL callee method. Pre-fix
// (sema half): compiled clean. Asserts the diagnostic TEXT.
// ===========================================================================
BEZEL_TEST(xmod_refout_prop_visibility_2454,
           private_set_imported_property_rejected_as_ref_arg) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2454_t1_" + std::to_string(std::rand());
    TempFile2454 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2454 aObj((dir / (stem + "_a.o")).string());
    writeSource2454(aSrc.path,
                    "module PrivSetLib2454T1; "
                    "public class PrivSetClass2454T1 "
                    "  private _n: int := 1; "
                    "  public property Name: int "
                    "    get => _n; "
                    "    private set => _n := value; "
                    "  end Name; "
                    "end PrivSetClass2454T1;");
    BEZEL_EXPECT_TRUE(compileToObj2454(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;  // toolchain unavailable

    std::string bSrc =
        "module App2454T1; import PrivSetLib2454T1; "
        "public class C "
        "  public static method Bump(ref v: int) "
        "    v := v + 1; "
        "  end Bump; "
        "  public static method Main(): int "
        "    using var d: PrivSetClass2454T1 := new PrivSetClass2454T1(); "
        "    C.Bump(ref d.Name); "
        "    return 0; "
        "  end Main; "
        "end C;";
    TempFile2454 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2454 bExe((dir / (stem + "_b")).string());
    writeSource2454(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2454(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("`ref` argument") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("not accessible") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("13.6") != std::string::npos);
}

// ===========================================================================
// T2 — the SAME `private set` shape as T1, but as an `out` argument. §22.3:
// `out` still requires the setter (the write-back always calls it), so this
// must be rejected identically to `ref` — isolates the `kind` text ("out"
// vs "ref") in the diagnostic and the out-specific call site.
// ===========================================================================
BEZEL_TEST(xmod_refout_prop_visibility_2454,
           private_set_imported_property_rejected_as_out_arg) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2454_t2_" + std::to_string(std::rand());
    TempFile2454 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2454 aObj((dir / (stem + "_a.o")).string());
    writeSource2454(aSrc.path,
                    "module PrivSetLib2454T2; "
                    "public class PrivSetClass2454T2 "
                    "  private _n: int := 1; "
                    "  public property Name: int "
                    "    get => _n; "
                    "    private set => _n := value; "
                    "  end Name; "
                    "end PrivSetClass2454T2;");
    BEZEL_EXPECT_TRUE(compileToObj2454(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2454T2; import PrivSetLib2454T2; "
        "public class C "
        "  public static method Produce(out v: int) "
        "    v := 5; "
        "  end Produce; "
        "  public static method Main(): int "
        "    using var d: PrivSetClass2454T2 := new PrivSetClass2454T2(); "
        "    C.Produce(out d.Name); "
        "    return 0; "
        "  end Main; "
        "end C;";
    TempFile2454 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2454 bExe((dir / (stem + "_b")).string());
    writeSource2454(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2454(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("`out` argument") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("not accessible") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("13.6") != std::string::npos);
}

// ===========================================================================
// T3 — a GETTER-ONLY (no setter at all) imported property as a `ref`
// argument. Isolates the hasSetter half (the #2412 side of this fix) from
// T1/T2's visibility half.
// ===========================================================================
BEZEL_TEST(xmod_refout_prop_visibility_2454,
           getter_only_imported_property_rejected_as_ref_arg) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2454_t3_" + std::to_string(std::rand());
    TempFile2454 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2454 aObj((dir / (stem + "_a.o")).string());
    writeSource2454(aSrc.path,
                    "module GetterOnlyLib2454T3; "
                    "public class GetterOnlyClass2454T3 "
                    "  public property Name: int => 1; "
                    "end GetterOnlyClass2454T3;");
    BEZEL_EXPECT_TRUE(compileToObj2454(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2454T3; import GetterOnlyLib2454T3; "
        "public class C "
        "  public static method Bump(ref v: int) "
        "    v := v + 1; "
        "  end Bump; "
        "  public static method Main(): int "
        "    using var d: GetterOnlyClass2454T3 := new "
        "GetterOnlyClass2454T3(); "
        "    C.Bump(ref d.Name); "
        "    return 0; "
        "  end Main; "
        "end C;";
    TempFile2454 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2454 bExe((dir / (stem + "_b")).string());
    writeSource2454(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2454(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("`ref` argument") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("has no setter") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("23.3") != std::string::npos);
}

// ===========================================================================
// T4 — the SAME getter-only shape as T3, but as `out`. §22.3: `out` also
// requires the setter (it never reads, but it always writes back), so this
// must be rejected too, even though `out` needs no getter.
// ===========================================================================
BEZEL_TEST(xmod_refout_prop_visibility_2454,
           getter_only_imported_property_rejected_as_out_arg) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2454_t4_" + std::to_string(std::rand());
    TempFile2454 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2454 aObj((dir / (stem + "_a.o")).string());
    writeSource2454(aSrc.path,
                    "module GetterOnlyLib2454T4; "
                    "public class GetterOnlyClass2454T4 "
                    "  public property Name: int => 1; "
                    "end GetterOnlyClass2454T4;");
    BEZEL_EXPECT_TRUE(compileToObj2454(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2454T4; import GetterOnlyLib2454T4; "
        "public class C "
        "  public static method Produce(out v: int) "
        "    v := 5; "
        "  end Produce; "
        "  public static method Main(): int "
        "    using var d: GetterOnlyClass2454T4 := new "
        "GetterOnlyClass2454T4(); "
        "    C.Produce(out d.Name); "
        "    return 0; "
        "  end Main; "
        "end C;";
    TempFile2454 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2454 bExe((dir / (stem + "_b")).string());
    writeSource2454(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2454(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("`out` argument") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("has no setter") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("23.3") != std::string::npos);
}

// ===========================================================================
// T5 — ACCEPT-CONTROL: an explicit `public set` imported property as a
// `ref` argument to a LOCAL callee method. Compiles AND RUNS, asserting the
// round-trip value (1 -> 2). Exercises BOTH halves of this fix together:
// the sema accept (§13.6, no restriction) and the codegen write-back fix
// (lowerImportedPropertySetValue fallback).
// ===========================================================================
BEZEL_TEST(xmod_refout_prop_visibility_2454,
           public_set_imported_property_accepted_as_ref_arg_and_writes) {
    int r = compileImportAndRun2454(
        "module PubSetLib2454T5; "
        "public class PubSetClass2454T5 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    public set => _n := value; "
        "  end Name; "
        "end PubSetClass2454T5;",
        "module App2454T5; import PubSetLib2454T5; "
        "public class C "
        "  public static method Bump(ref v: int) "
        "    v := v + 1; "
        "  end Bump; "
        "  public static method Main(): int "
        "    using var d: PubSetClass2454T5 := new PubSetClass2454T5(); "
        "    C.Bump(ref d.Name); "
        "    return d.Name - 2; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T6 — ACCEPT-CONTROL: a WRITE-ONLY (no getter) imported property as an
// `out` argument. §22.3: "for `out` no getter call occurs" — a write-only
// property IS a legal `out` target even though (per the SCOPE note above)
// it would not be a legal `ref` target. Compiles AND RUNS, asserting the
// value actually written (via a getter method on the class itself, since
// the property has no getter to read back through).
// ===========================================================================
BEZEL_TEST(xmod_refout_prop_visibility_2454,
           write_only_imported_property_accepted_as_out_arg_and_writes) {
    int r = compileImportAndRun2454(
        "module WriteOnlyLib2454T6; "
        "public class WriteOnlyClass2454T6 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    set => _n := value; "
        "  end Name; "
        "  public method Peek(): int => _n; "
        "end WriteOnlyClass2454T6;",
        "module App2454T6; import WriteOnlyLib2454T6; "
        "public class C "
        "  public static method Produce(out v: int) "
        "    v := 42; "
        "  end Produce; "
        "  public static method Main(): int "
        "    using var d: WriteOnlyClass2454T6 := new WriteOnlyClass2454T6(); "
        "    C.Produce(out d.Name); "
        "    return d.Peek() - 42; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T7 — `protected set`, IMPORTED, `ref` argument, from an UNRELATED local
// class (no inheritance relationship). §13.1: protected = "the declaring
// class and all classes derived from it" — an unrelated class qualifies as
// neither, so this must be rejected exactly like `private set`.
// ===========================================================================
BEZEL_TEST(xmod_refout_prop_visibility_2454,
           protected_set_imported_property_rejected_as_ref_from_unrelated) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2454_t7_" + std::to_string(std::rand());
    TempFile2454 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2454 aObj((dir / (stem + "_a.o")).string());
    writeSource2454(aSrc.path,
                    "module ProtSetLib2454T7; "
                    "public class ProtSetBase2454T7 "
                    "  private _n: int := 1; "
                    "  public property Name: int "
                    "    get => _n; "
                    "    protected set => _n := value; "
                    "  end Name; "
                    "end ProtSetBase2454T7;");
    BEZEL_EXPECT_TRUE(compileToObj2454(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2454T7; import ProtSetLib2454T7; "
        "public class C "
        "  public static method Bump(ref v: int) "
        "    v := v + 1; "
        "  end Bump; "
        "  public static method Main(): int "
        "    using var d: ProtSetBase2454T7 := new ProtSetBase2454T7(); "
        "    C.Bump(ref d.Name); "
        "    return 0; "
        "  end Main; "
        "end C;";
    TempFile2454 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2454 bExe((dir / (stem + "_b")).string());
    writeSource2454(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2454(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("`ref` argument") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("not accessible") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("13.6") != std::string::npos);
}

// ===========================================================================
// T8 — ACCEPT: `protected set`, IMPORTED, `ref` argument, from a LOCAL
// SUBCLASS's own method (the false-reject axis §13.1 grants — "the
// declaring class and all classes derived from it", no module qualifier).
// Compiles AND RUNS. Asserts a DELTA (`after - before == 1`), not an
// absolute value: an UNRELATED, separately-filed bug (a local subclass of
// an imported base does not run the imported base's field initializers)
// would otherwise contaminate an absolute-value assertion here with a
// failure this fix did not cause and does not need to fix — the delta
// isolates exactly what this fix is responsible for (the `ref` write
// itself taking effect) from that unrelated initial-value bug.
// ===========================================================================
BEZEL_TEST(xmod_refout_prop_visibility_2454,
           protected_set_imported_property_accepted_as_ref_from_subclass) {
    int r = compileImportAndRun2454(
        "module ProtSetLib2454T8; "
        "public class ProtSetBase2454T8 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    protected set => _n := value; "
        "  end Name; "
        "end ProtSetBase2454T8;",
        "module App2454T8; import ProtSetLib2454T8; "
        "public class Derived2454T8: ProtSetBase2454T8 "
        "  public method BumpIt() "
        "    Bump(ref this.Name); "
        "  end BumpIt; "
        "  private method Bump(ref v: int) "
        "    v := v + 1; "
        "  end Bump; "
        "end Derived2454T8; "
        "public class C "
        "  public static method Main(): int "
        "    using var d: Derived2454T8 := new Derived2454T8(); "
        "    var before: int := d.Name; "
        "    d.BumpIt(); "
        "    var after: int := d.Name; "
        "    return after - before - 1; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T9 — `internal set`, IMPORTED, `ref` argument. §13.1: internal = "code in
// the same module" — the importing module is by definition a different
// module, so this must always be rejected.
// ===========================================================================
BEZEL_TEST(xmod_refout_prop_visibility_2454,
           internal_set_imported_property_rejected_as_ref_arg) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2454_t9_" + std::to_string(std::rand());
    TempFile2454 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2454 aObj((dir / (stem + "_a.o")).string());
    writeSource2454(aSrc.path,
                    "module IntSetLib2454T9; "
                    "public class IntSetClass2454T9 "
                    "  private _n: int := 1; "
                    "  public property Name: int "
                    "    get => _n; "
                    "    internal set => _n := value; "
                    "  end Name; "
                    "end IntSetClass2454T9;");
    BEZEL_EXPECT_TRUE(compileToObj2454(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2454T9; import IntSetLib2454T9; "
        "public class C "
        "  public static method Bump(ref v: int) "
        "    v := v + 1; "
        "  end Bump; "
        "  public static method Main(): int "
        "    using var d: IntSetClass2454T9 := new IntSetClass2454T9(); "
        "    C.Bump(ref d.Name); "
        "    return 0; "
        "  end Main; "
        "end C;";
    TempFile2454 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2454 bExe((dir / (stem + "_b")).string());
    writeSource2454(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2454(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("`ref` argument") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("not accessible") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("13.6") != std::string::npos);
}

// ===========================================================================
// T10 — REJECT-CONTROL: the SAME-MODULE (local) equivalent of T1's shape is
// still, correctly, rejected — unaffected by the fix (the fix only adds to
// the `else`/imported branch; the local `pd`-gated branch, unchanged, has
// enforced this since before #2454). Drives the Driver directly (no
// dependency object needed).
// ===========================================================================
BEZEL_TEST(xmod_refout_prop_visibility_2454,
           local_private_set_property_still_rejected_as_ref_control) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2454_t10_" + std::to_string(std::rand());
    TempFile2454 src((dir / (stem + ".bzl")).string());
    TempFile2454 exe((dir / stem).string());
    writeSource2454(src.path,
                    "module LocalMod2454T10; "
                    "public class PrivSetClass2454T10 "
                    "  private _n: int := 1; "
                    "  public property Name: int "
                    "    get => _n; "
                    "    private set => _n := value; "
                    "  end Name; "
                    "end PrivSetClass2454T10; "
                    "public class C "
                    "  public static method Bump(ref v: int) "
                    "    v := v + 1; "
                    "  end Bump; "
                    "  public static method Main(): int "
                    "    using var d: PrivSetClass2454T10 := "
                    "new PrivSetClass2454T10(); "
                    "    C.Bump(ref d.Name); "
                    "    return 0; "
                    "  end Main; "
                    "end C;");

    std::string errPath = (dir / (stem + "_err.txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        success = driver.compileFile(src.path, exe.path, {}).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    std::ifstream in(errPath);
    std::string err((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());

    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("`ref` argument") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("not accessible") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("13.6") != std::string::npos);
}

}  // namespace bezel::module
