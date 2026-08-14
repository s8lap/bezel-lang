// fix #2449 (§11.2/§23.4, v0.235): the imported-base half of #2442's own
// residual gap. #2442 taught `Emitter::findIndexer`/`classIndexerImplementing`
// (LlvmBackend_Assign.cpp) to walk the base-class chain, so a class satisfying
// an interface indexer obligation only through an INHERITED indexer became a
// valid candidate — but ONLY for a LOCAL base class: the candidate was
// accepted only when it carried a genuine `ast::PropertyDecl`, since
// `ensureIndexerGetterFn`/`-SetterFn` require one unconditionally and an
// IMPORTED PropertySymbol never has one (ModuleLoader reconstructs no AST —
// the same `#2374`-class gap #2387/#2414/#2417 are all separate instances
// of). An indexer inherited ONLY from an IMPORTED base class therefore
// compiled clean per #2437/#2442's own diagnostic wording ("...on `t` OR AN
// ANCESTOR CLASS...") right up until it hit that gate — and then fell
// through to "does not implement" instead, a FALSE REJECT (filed as #2449
// immediately after #2442 landed, with a mechanical cross-module probe
// confirming reject-not-crash).
//
// THE FIX (this commit): `Emitter::findIndexer`'s candidate gate
// (`indexerCandidateWireable`, LlvmBackend_impl.hpp) now also accepts a
// non-virtual, non-interface-declared IMPORTED indexer — mirroring exactly
// the wireability #2414's `declareImportedIndexerGetters`/`-Setters` already
// grant for the ORDINARY (non-itable) indexer-access path. Pass D
// (`declareVtables`, LlvmBackend_Setup.cpp) resolves the actual itable-slot
// function through the new `Emitter::indexerItableAccessor`, which reuses
// the ALREADY-DECLARED cross-module ABI stub
// (`importedIndexerGetterFns_`/`-SetterFns_`, populated by
// `declareImportedIndexerGetters`/`-Setters`, called earlier in
// `declareVtables`, before Pass D) instead of trying to build a body from a
// nonexistent AST. `Resolver::checkAbstractImplementations`'s sema mirror
// (TypeResolution_Checks.cpp) is updated in this SAME commit with the
// identical wireability gate and reads the reconstructed `hasSetter` bit
// (`ctx_.importedPropertyHasSetter`, #2400/#2414's own metadata — no NEW
// metadata field was added for this fix) instead of an AST accessor-presence
// check for an imported candidate.
//
// A VIRTUAL (or interface-declared) imported indexer stays EXCLUDED on
// purpose: it dispatches through a vtable/itable slot, not this direct-call
// ABI, and #2414's own declareImportedIndexerGetters/-Setters never declare
// a stub for one — accepting it here would trade #2449's false reject for a
// false ACCEPT of a program still unwireable (T4 below pins this).
//
// ★ FOLLOW-UP (fix #3204, v0.254): the VIRTUAL / OVERRIDE half of the
// exclusion is now lifted. Fix #3190 added a vtable-forward thunk
// (`importedVirtualAccessorSlotFn`, LlvmBackend_Assign.cpp:5372) that
// emits a small forwarder loading `vt$<Owner>` and tail-calling the
// slot — so the itable slot gets a real function instead of
// `ConstantPointerNull`. T4 below is updated to pin the remaining
// ABSTRACT case (the only reject path that survives: an abstract
// member has no body in the exporter, its vtable slot is legitimately
// null, and the forwarder would forward into the same null-call crash
// #2438 exists to prevent). The newly-accepted virtual/override
// shape is its own regression test in
// tests/module/xmod_inherited_virtual_indexer_iface_3204_test.cpp.
//
// CONFOUND NOTE (found and filed separately as #2452, NOT this fix's
// scope): a LOCAL class deriving from an IMPORTED base does not currently
// run the base's field initializers or constructor — `_v := 10` on an
// imported base is silently left at 0 when read through a LOCAL derived
// instance. This is a general cross-module CONSTRUCTION defect, unrelated
// to indexers/interfaces (confirmed to affect a plain inherited METHOD call
// with no indexer or interface involved at all). It does NOT undermine this
// fix's own tests: T1/T4 below use a getter body that returns a value
// INDEPENDENT of any base instance field (mirrors
// xmod_itable_prop_inherited_import_2387_test.cpp's own T1, which uses the
// identical `=> 1` state-independent pattern for exactly this reason); T2
// below uses SET-then-GET, so the setter's absolute write masks whatever
// (wrong) initial state #2452 leaves the field in — the returned value is
// still a mechanical proof that both the setter AND the getter dispatch
// through the SAME real storage on the imported base instance.
//
// SCOPE: mirrors #2414's own scope boundary — the non-generic indexer
// shape is covered for both the non-virtual direct-call ABI (#2414)
// and the virtual vtable-forward ABI (#3190 + #3204). A generic
// imported base, or an indexer overloaded by arity ACROSS the
// CLASS+ARITY-keyed cross-module ABI (#2414's own documented
// simplification), are not exercised here.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2449 {
    std::string path;
    explicit TempFile2449(std::string p) : path(std::move(p)) {}
    ~TempFile2449() { std::remove(path.c_str()); }
};

void writeSource2449(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2449(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile lib -> object, compile app (importing lib) -> exe, run it in a
// SEPARATE process (fork-isolated per tests/README.md's AOT harness
// discipline), return its exit code (-1 on compile/link/run failure, -2 if
// the dependency itself could not be produced). Mirrors #2387/#2414's own
// compileImportAndRun helpers.
int compileImportAndRun2449(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2449_" + std::to_string(std::rand());
    TempFile2449 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2449 aObj((dir / (stem + "_a.o")).string());
    writeSource2449(aSrc.path, libSrc);
    if (!compileToObj2449(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2449 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2449 bExe((dir / stem).string());
    writeSource2449(bSrc.path, appSrc);
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

// Compile lib -> object, then compile app -> exe WITHOUT running, capturing
// the driver's stderr (fd 2) into `err` and returning compile success.
// Mirrors xmod_getter_only_prop_2412_test.cpp's compileImportCaptureErr2412
// (itself mirroring #2380/#1943) — `CompileResult` (Driver.hpp) carries only
// `success`/`errorCount`, no diagnostic text, so a text assertion needs the
// stderr capture. Used for the reject-control tests, which need to assert on
// diagnostic TEXT specifically (a coarse compile-success check alone cannot
// distinguish a genuine compile-time reject from a runtime crash the AOT
// oracle might synthesize an Error diagnostic for — see this repo's lane
// protocol and #2438's own test file for the documented trap; here there is
// no run at all, so the distinction instead guards against asserting on the
// WRONG diagnostic, e.g. a getter-only-vs-readwrite reject read as a
// does-not-implement one or vice versa).
bool compileImportCaptureErr2449(const std::string& libSrc,
                                 const std::string& appSrc, std::string& err) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2449_c_" + std::to_string(std::rand());
    TempFile2449 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2449 aObj((dir / (stem + "_a.o")).string());
    writeSource2449(aSrc.path, libSrc);
    if (!compileToObj2449(aSrc.path, aObj.path)) return false;
    if (!std::ifstream(aObj.path).good()) return false;

    TempFile2449 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2449 bExe((dir / (stem + "_b")).string());
    writeSource2449(bSrc.path, appSrc);

    std::string errPath =
        (dir / ("bz2449_err_" + std::to_string(std::rand()) + ".txt"))
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
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        success = driver.compileFile(bSrc.path, bExe.path, deps).success;
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

}  // namespace

// ===========================================================================
// T1 — the #2449 repro's GET half: a READ-ONLY interface obligation satisfied
// only through an indexer INHERITED from an IMPORTED base class. Getter body
// is state-INDEPENDENT (`100 + i`, no field read) specifically to sidestep
// #2452's separate imported-base-construction confound — see file header.
// Pre-fix: "does not implement interface `IBox2449T1`'s indexer" (a false
// reject — #2438's diagnostic, since #2442's own local-decl gate excluded
// this candidate). Post-fix: dispatches through the itable to the imported
// base's real getter.
// ===========================================================================
BEZEL_TEST(indexer_inherited_import_2449,
           readonly_get_inherited_from_imported_base_dispatches_11_2_23_4) {
    int r = compileImportAndRun2449(
        "module IdxBaseLib2449T1; "
        "public class Base2449T1 "
        "  public property [i: int]: int => 100 + i; "
        "end Base2449T1;",
        "module App2449T1; import IdxBaseLib2449T1; "
        "public interface IBox2449T1 "
        "  readonly property [i: int]: int; "
        "end IBox2449T1; "
        "public class Derived2449T1: Base2449T1, IBox2449T1 "
        "end Derived2449T1; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new Derived2449T1(); "
        "    var ib: IBox2449T1 := d; "
        "    return ib[7]; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 107);
}

// ===========================================================================
// T2 — the READ-WRITE half: SET then GET through the interface, both
// dispatching to the SAME imported base instance's real storage (`_v`). This
// is the shape that is unaffected by #2452 (the setter's absolute write
// masks whatever wrong initial value #2452 would otherwise leave `_v` at) —
// and mechanically proves BOTH accessors are wired, not just the getter:
// `ib[5] := 42` sets `_v := 42 - 5 = 37`; `ib[0]` reads `_v (37) + 0 = 37`.
// ===========================================================================
BEZEL_TEST(indexer_inherited_import_2449,
           readwrite_set_then_get_inherited_from_imported_base_11_2_23_4) {
    int r = compileImportAndRun2449(
        "module IdxBaseLib2449T2; "
        "public class Base2449T2 "
        "  private _v: int := 10; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end Base2449T2;",
        "module App2449T2; import IdxBaseLib2449T2; "
        "public interface IBox2449T2 "
        "  property [i: int]: int; "
        "end IBox2449T2; "
        "public class Derived2449T2: Base2449T2, IBox2449T2 "
        "end Derived2449T2; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new Derived2449T2(); "
        "    var ib: IBox2449T2 := d; "
        "    ib[5] := 42; "        // _v := 37
        "    return ib[0]; "       // 37
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 37);
}

// ===========================================================================
// T3 — REJECT control: a GETTER-ONLY indexer inherited from an imported
// base against a READ-WRITE local interface obligation. Proves the
// accessor-completeness check (§11.2/§23.4, the #2417/#2438 sibling) still
// fires for an IMPORTED candidate — reading `ctx_.importedPropertyHasSetter`
// (the reconstructed metadata bit) instead of an AST accessor scan, since an
// imported candidate has no AST to scan.
// ===========================================================================
BEZEL_TEST(indexer_inherited_import_2449,
           getter_only_imported_base_vs_readwrite_obligation_rejected) {
    std::string err;
    bool success = compileImportCaptureErr2449(
        "module IdxBaseLib2449T3; "
        "public class Base2449T3 "
        "  public property [i: int]: int => 100 + i; "
        "end Base2449T3;",
        "module App2449T3; import IdxBaseLib2449T3; "
        "public interface IBox2449T3 "
        "  property [i: int]: int; "
        "end IBox2449T3; "
        "public class Derived2449T3: Base2449T3, IBox2449T3 "
        "end Derived2449T3; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new Derived2449T3(); "
        "    var ib: IBox2449T3 := d; "
        "    return ib[0]; "
        "  end Main; "
        "end C;",
        err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("getter-only") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("23.4") != std::string::npos);
}

// ===========================================================================
// T4 — REJECT control: an ABSTRACT indexer on an imported base is the
// remaining reject path. As of fix #3204 the earlier "virtual / override
// inherited from an imported base" case is no longer a reject (the
// vtable-forward thunk #3190 added makes it wireable through the itable
// slot, and the new behaviour is pinned by
// tests/module/xmod_inherited_virtual_indexer_iface_3204_test.cpp). The
// ABSTRACT case, by contrast, has no body in the exporter, so its own
// `vt$<Owner>` slot is legitimately null and the forwarder would forward
// into the same null-call crash #2438/#2442 exist to prevent — the
// sema gate has to reject it. Pin that the narrowness survives here.
// ===========================================================================
BEZEL_TEST(indexer_inherited_import_2449,
           abstract_imported_base_indexer_stays_rejected_11_2_23_4) {
    std::string err;
    bool success = compileImportCaptureErr2449(
        "module IdxBaseLib2449T4; "
        "public abstract class Base2449T4 "
        "  public abstract property [i: int]: int "
        "    get; "
        "    set; "
        "  end; "
        "end Base2449T4;",
        "module App2449T4; import IdxBaseLib2449T4; "
        "public interface IBox2449T4 "
        "  readonly property [i: int]: int; "
        "end IBox2449T4; "
        "public class Derived2449T4: Base2449T4, IBox2449T4 "
        "end Derived2449T4; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new Derived2449T4(); "
        "    var ib: IBox2449T4 := d; "
        "    return ib[0]; "
        "  end Main; "
        "end C;",
        err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("does not implement") != std::string::npos);
}

// ===========================================================================
// T5 — CONTROL: a LOCAL base class (no import at all) satisfying the same
// shape — guards against #2449's imported-candidate branch accidentally
// short-circuiting or otherwise interfering with #2442's own already-fixed
// LOCAL-base path (same-module inherited indexer dispatch).
// ===========================================================================
BEZEL_TEST(indexer_inherited_import_2449,
           local_base_control_unaffected_by_imported_candidate_gate) {
    bezel::Driver driver;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2449_t5_" + std::to_string(std::rand());
    TempFile2449 src((dir / (stem + ".bzl")).string());
    TempFile2449 exe((dir / stem).string());
    writeSource2449(src.path,
                    "module LocalCtl2449T5; "
                    "public interface IBoxL2449T5 "
                    "  property [i: int]: int; "
                    "end IBoxL2449T5; "
                    "public class BaseL2449T5 "
                    "  private _v: int := 10; "
                    "  public property [i: int]: int "
                    "    get => _v + i; "
                    "    set => _v := value - i; "
                    "  end; "
                    "end BaseL2449T5; "
                    "public class DerivedL2449T5: BaseL2449T5, IBoxL2449T5 "
                    "end DerivedL2449T5; "
                    "public class C "
                    "  public static method Main(): int "
                    "    using var d := new DerivedL2449T5(); "
                    "    var ib: IBoxL2449T5 := d; "
                    "    ib[5] := 42; "
                    "    return ib[0]; "
                    "  end Main; "
                    "end C;");
    BEZEL_EXPECT_TRUE(driver.compileFile(src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 37);
}

}  // namespace bezel::module
