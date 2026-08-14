// fix #3204 (§11.2 / §23.4 / §2.8.2 clause 1 / §2.8.3 rule 4, v0.254):
// the 4th site of the #3190/#2837/#3199 export-gate asymmetry. A VIRTUAL
// indexer inherited from an IMPORTED base class must satisfy an interface
// indexer obligation. The non-virtual twin works (#2449); the only
// difference between the accepting and rejecting programs is the word
// `virtual` on the imported base's indexer.
//
// ── WHAT THE AUDIT SAID, AND WHAT MEASUREMENT FOUND ────────────────────────
//
// `candWireable` (sema/TypeResolution_Checks.cpp, the gate this issue
// lands) deliberately excluded `virtual`/`override`/`abstract` from
// candidates at the time of #2449, on the ground that the direct-call
// cross-module ABI #2414 declares (`bz$<Class>$idxget$<arity>`) is never
// emitted for a virtual member and so a virtual candidate would silently
// install a null itable slot. That verdict was correct THEN: the direct-
// call ABI is the only ABI #2449 had. Fix #3190 widened the dispatch
// surface to a DIFFERENT ABI for the virtual shape — `importedVirtual
// AccessorSlotFn` emits a small forwarding thunk that loads the
// `vt$<Owner>` slot and tail-calls it (LlvmBackend_Assign.cpp:5372), so a
// virtual imported accessor IS now wireable, through the vtable slot
// rather than the direct-call ABI. The original verdict is no longer
// complete: the virtual shape is no longer "unwireable", it is merely
// "wireable through a different ABI". #3204 widens the gate to admit
// virtual/override (deliberately still rejecting `abstract` — an abstract
// member has no body in the exporter, so its own `vt$<Owner>` slot is
// legitimately null, and an itable slot filled by forwarding through it
// would relocate the identical null-call crash; #2449's T4, which the
// per-fix review of #3190 filed against here, is now folded into this
// commitment by becoming a new "abstract stays rejected" test).
//
// Sibling test #2449's T4 (it pinned the old "virtual=rejected" verdict
// at the time #2449 landed) is wrong as of #3190 and is UPDATED in this
// commit to pin the new expected behaviour (the inherited virtual
// indexer satisfies the interface obligation, since the itable slot is
// now backed by the vtable-forward thunk). The ABSTRACT half of the
// control becomes T4_REORG below — the same shape with `abstract`
// instead of `virtual`, which is now the only remaining reject path.
//
// ── THE FIX (this commit) ──────────────────────────────────────────────────
//
// 1. `candWireable` (sema, TypeResolution_Checks.cpp) — accept virtual /
//    override for an IMPORTED candidate when codegen can wire it. The
//    local (`ast::PropertyDecl` present) branch is unchanged; the
//    abstract branch stays excluded (see above).
//
// 2. `indexerCandidateWireable` (codegen, LlvmBackend_impl.hpp) — the
//    sema/codegen mirror that the iface-indexer check deliberately
//    follows (see #2449's commit message), updated identically so the
//    itable site the check selects from agrees with what the check
//    accepts. The codegen side's `callable` answer is now
//    `ensureIndexerGetterFn`/`-SetterFn`, which already route an
//    imported virtual candidate through `importedVirtualAccessorSlotFn`
//    (the vtable-forward thunk #3190 added).
//
// 3. `indexerItableAccessor` (codegen, LlvmBackend_Assign.cpp) — the
//    resolver for the itable slot, which used to look up ONLY the
//    `importedIndexerGetterFns_`/`-SetterFns_` maps populated by #2414's
//    direct-call ABI declarators. Those maps are EMPTY for a virtual
//    member (the declarators themselves skip
//    `if (propGetSlot_.count(ps)) continue;`, see #2545), so the lookup
//    fell through to null and installed `ConstantPointerNull` into the
//    itable slot — the exact null-call shape #2438's own reject exists
//    to prevent at the sema gate. The fix routes the imported path
//    through `ensureIndexerGetterFn`/`-SetterFn` instead, which #3190
//    already taught to return the vtable-forward thunk for the same
//    shape. The local branch is unchanged.
//
// ── MEASURED, bezelc -c lib.bzl -o lib.o then --import lib.o app.bzl ──────
//
//   shape                                       pre-fix     post-fix
//   D5 has IFace, VBase.indexer is VIRTUAL,
//     read via IFace (the issue's repro)        REJECT 107
//   D5 has IFace, VBase.indexer is VIRTUAL,
//     set via IFace then read via IFace         REJECT 92
//   D5 has IFace, VBase.indexer is VIRTUAL,
//     read via base-typed reference             (was SIGSEGV via #3190; 107)
//   D5 has IFace, VBase.indexer is ABSTRACT,
//     read via IFace                            REJECT  REJECT (kept)
//   D5 has IFace, VBase.indexer is NON-VIRTUAL,
//     read via IFace (the #2449 twin)           107     107 (kept)
//
// ── CONTROLS (T5-T6) ───────────────────────────────────────────────────────
//
// T5  ABSTRACT indexer on imported base vs interface obligation — STAYS
//     a "does not implement" reject. An abstract member has no body
//     in the exporter, so its vtable slot is legitimately null, and
//     the vtable-forward thunk would forward into the same null-call
//     crash. The fix's only narrowness is on this one keyword.
// T6  LOCAL twin (no import) — the SINGLE-MODULE control. The inherited
//     virtual indexer satisfied the interface obligation even before
//     this fix (#2442's own base-walk landed first), so the LOCAL
//     case is the prove-the-shape-is-legal control and must agree with
//     the imported post-fix value.

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
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile3204 {
    std::string path;
    explicit TempFile3204(std::string p) : path(std::move(p)) {}
    ~TempFile3204() { std::remove(path.c_str()); }
};

void writeSource3204(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj3204(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile lib -> object, compile app (importing lib) -> exe, run it in a
// SEPARATE process (fork-isolated per tests/README.md's AOT harness
// discipline), return its exit code. -1 on compile/link/run failure, -2
// if the dependency itself could not be produced.
int compileImportAndRun3204(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3204_" + std::to_string(std::rand());
    TempFile3204 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3204 aObj((dir / (stem + "_a.o")).string());
    writeSource3204(aSrc.path, libSrc);
    if (!compileToObj3204(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile3204 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3204 bExe((dir / stem).string());
    writeSource3204(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
             .success)
        return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Compile lib -> object, then compile app -> exe WITHOUT running, capturing
// stderr. A text assertion needs the raw diagnostic text; `CompileResult`
// carries only `success`/`errorCount`.
bool compileImportCaptureErr3204(const std::string& libSrc,
                                 const std::string& appSrc,
                                 std::string& err) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3204c_" + std::to_string(std::rand());
    TempFile3204 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3204 aObj((dir / (stem + "_a.o")).string());
    writeSource3204(aSrc.path, libSrc);
    if (!compileToObj3204(aSrc.path, aObj.path)) return false;
    if (!std::ifstream(aObj.path).good()) return false;

    TempFile3204 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3204 bExe((dir / (stem + "_b")).string());
    writeSource3204(bSrc.path, appSrc);

    std::string errPath =
        (dir / ("bz3204_err_" + std::to_string(std::rand()) + ".txt"))
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
        success = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path,
                                                  deps)
                      .success;
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

// The single-module twin runner.
int compileLocalAndRun3204(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3204l_" + std::to_string(std::rand());
    TempFile3204 aSrc((dir / (stem + ".bzl")).string());
    TempFile3204 aExe((dir / stem).string());
    writeSource3204(aSrc.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, aSrc.path, aExe.path).success)
        return -1;
    int status = std::system(aExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Library text for the imported cases. The base declares a virtual indexer
// using a getter body that is STATE-INDEPENDENT (`100 + i`) — the same
// guard #2449 used against #2452's imported-base-construction confound
// (a LOCAL class deriving from an IMPORTED base does not run the base's
// field initializers; a field-state-dependent body would read 0 and break
// the test). The setter twin (read-write shape) uses a backing field
// because the SET-then-GET shape has the setter's absolute write mask
// whatever initial state would otherwise be there.
const char* kReadonly3204 =
    "public class VBase3204 "
    "  public virtual property [i: int]: int => 100 + i; "
    "end VBase3204; ";

const char* kReadwrite3204 =
    "public class VBase3204 "
    "  private _v: int := 0; "
    "  public virtual property [i: int]: int "
    "    get => _v + i; "
    "    set => _v := value - i; "
    "  end; "
    "end VBase3204; ";

const char* kAbstract3204 =
    "public abstract class VBase3204 "
    "  public abstract property [i: int]: int "
    "    get; "
    "    set; "
    "  end; "
    "end VBase3204; ";

const char* kLibHead3204 = "module VIdxLib3204; ";

}  // namespace

// ===========================================================================
// T1 — the issue's repro: a derived class `D5` satisfies `ILocalIdx3`'s
// indexer obligation ONLY through the indexer inherited from an IMPORTED
// virtual base. The interface reference is the dispatch path. Read goes
// through the itable slot; the slot is now filled by the vtable-forward
// thunk #3190 added, so the read returns 100 + 7 = 107.
// Pre-fix: "does not implement" reject (false reject of a legal program —
// §2.8.3 rule 4, the imported answer must match the local one).
// Post-fix: 107.
// ===========================================================================
BEZEL_TEST(xmod_inherited_virtual_indexer_iface_3204,
           virtual_imported_base_indexer_satisfies_iface_11_2_23_4) {
    int r = compileImportAndRun3204(
        std::string(kLibHead3204) + kReadonly3204,
        "module App3204; import VIdxLib3204; "
        "public interface ILocalIdx3 "
        "  readonly property [i: int]: int; "
        "end ILocalIdx3; "
        "public class D5: VBase3204, ILocalIdx3 "
        "end D5; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D5(); "
        "    var ib: ILocalIdx3 := d; "
        "    return ib[7]; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 107);
}

// ===========================================================================
// T2 — the read-write twin: SET then GET through the interface. The
// SETTER's absolute write masks whatever base-instance state is
// observable (the read of `_v` happens AFTER the write), so this test
// mechanically proves both accessors dispatch through the SAME real
// storage on the imported base instance rather than one of them silently
// answering a constant. `ib[5] := 42` writes `_v := 37`; `ib[0]` reads
// `37 + 0 = 37`.
// ===========================================================================
BEZEL_TEST(xmod_inherited_virtual_indexer_iface_3204,
           virtual_imported_base_indexer_set_then_get_11_2_23_4) {
    int r = compileImportAndRun3204(
        std::string(kLibHead3204) + kReadwrite3204,
        "module App3204; import VIdxLib3204; "
        "public interface ILocalIdx3 "
        "  property [i: int]: int; "
        "end ILocalIdx3; "
        "public class D5: VBase3204, ILocalIdx3 "
        "end D5; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D5(); "
        "    var ib: ILocalIdx3 := d; "
        "    ib[5] := 42; "
        "    return ib[0]; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 37);
}

// ===========================================================================
// T3 — the BASE-TYPED reference shape (the §12.1 dispatch path, not the
// §23.4 itable path). The vtable-forward thunk #3190 already wired the
// base-typed-reference path; this test confirms the fix's itable route
// (T1/T2) agrees with the vtable route on the base-receiver shape —
// both now return 100 + 7 = 107. Together they prove the codegen AND
// sema moves agree (a one-sided fix would only resolve one path).
// ===========================================================================
BEZEL_TEST(xmod_inherited_virtual_indexer_iface_3204,
           virtual_imported_base_indexer_via_base_ref_12_1_23_4) {
    int r = compileImportAndRun3204(
        std::string(kLibHead3204) + kReadonly3204,
        "module App3204; import VIdxLib3204; "
        "public interface ILocalIdx3 "
        "  readonly property [i: int]: int; "
        "end ILocalIdx3; "
        "public class D5: VBase3204, ILocalIdx3 "
        "end D5; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D5(); "
        "    var b: VBase3204 := d; "
        "    return b[7]; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 107);
}

// ===========================================================================
// T4 — REJECT control: an ABSTRACT indexer on an imported base class
// against an interface indexer obligation. The fix's narrowness is on
// this one keyword: an abstract member has no body in the exporter, so
// `vt$<Owner>`'s slot is legitimately null, and the vtable-forward
// thunk would forward through it into the same null-call crash #2438
// exists to prevent at the sema gate. Stays a "does not implement"
// reject — proves the abstract branch is the only remaining reject
// path, not a wholesale widening of the wireability gate.
// ===========================================================================
BEZEL_TEST(xmod_inherited_virtual_indexer_iface_3204,
           abstract_imported_base_indexer_still_rejected_11_2_23_4) {
    std::string err;
    bool success = compileImportCaptureErr3204(
        std::string(kLibHead3204) + kAbstract3204,
        "module App3204; import VIdxLib3204; "
        "public interface ILocalIdx3 "
        "  readonly property [i: int]: int; "
        "end ILocalIdx3; "
        "public class D5: VBase3204, ILocalIdx3 "
        "end D5; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D5(); "
        "    var ib: ILocalIdx3 := d; "
        "    return ib[0]; "
        "  end Main; "
        "end C;",
        err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("does not implement") != std::string::npos);
}

// ===========================================================================
// T5 — LOCAL twin (no import) — the SINGLE-MODULE control. The inherited
// virtual indexer satisfied the interface obligation even before #3190
// (#2442's own base-walk landed first), so the LOCAL case is the
// prove-the-shape-is-legal control and must agree with the imported
// post-fix value.
// ===========================================================================
BEZEL_TEST(xmod_inherited_virtual_indexer_iface_3204,
           local_twin_control_11_2_23_4) {
    int r = compileLocalAndRun3204(
        "module Local3204; "
        "public interface ILocalIdx3 "
        "  readonly property [i: int]: int; "
        "end ILocalIdx3; "
        "public class VBase3204 "
        "  public virtual property [i: int]: int => 100 + i; "
        "end VBase3204; "
        "public class D5: VBase3204, ILocalIdx3 "
        "end D5; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D5(); "
        "    var ib: ILocalIdx3 := d; "
        "    return ib[7]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r, 107);
}

}  // namespace bezel::module
