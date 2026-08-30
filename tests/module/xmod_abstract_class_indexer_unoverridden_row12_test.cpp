// Regression coverage for the 2026-08-13 duplication-census row 12 finding
// (`classIndexerMatching` never applied the `indexerCandidateWireable` filter
// that `findIndexer` — the itable-route twin — already applies, LlvmBackend_impl.hpp
// / LlvmBackend_Assign.cpp) and the SEMA gap the reachability probe surfaced.
//
// §12.5 "Abstract classes and members", v0.286, states verbatim: "`abstract`
// on a member declares it without a body; every concrete subclass must
// override it." §23.4 "Indexers" states an indexer "participate[s] in
// `virtual`/`override`/`sealed`/`abstract` like methods" — no textual
// carve-out excluding indexers from §12.5's override obligation.
//
// MEASURED (this cycle): a concrete class inheriting, without overriding, an
// IMPORTED abstract-class indexer compiles with ZERO diagnostics on both
// sides of the module boundary, then crashes at runtime — `d[5]` virtually
// dispatches through the concrete class's OWN vtable slot for the indexer,
// which was never filled (the only candidate `classIndexerMatching` finds on
// the base-class walk is the unimplemented abstract ancestor's indexer,
// which `ensureIndexerGetterFn`/`importedVirtualAccessorSlotFn`
// (LlvmBackend_Assign.cpp) decline via their own independent abstract-
// modifier guard, leaving the slot at its `ConstantPointerNull` default).
// Under the PLAIN build this is a direct SIGSEGV (exit 139). Under `--full`
// (BEZEL_AOT_FORCE_SANITIZE=address) ASan's own fault handler intercepts the
// SEGV and terminates via a NORMAL `_exit()` instead — this repo gates on
// BOTH oracles (#2633), so the assertion below accepts either shape; see its
// own comment.
//
// ROOT CAUSE: `TypeResolution_Checks.cpp`'s abstract-member-implementation
// walk (~lines 4614-4797) explicitly `continue`s past EVERY indexer
// regardless of whether the declaring ancestor is a class or an interface —
// only the INTERFACE half is tracked (`ifaceIndexerObligations`, gated
// `if (curIsIface)`, backed by #2417/#2437/#2442/#2449's ~150-line matching
// machinery that already mirrors codegen's `findIndexer`/
// `indexerCandidateWireable` exactly). A CLASS-declared abstract indexer has
// no analogous check. This is `src/sema/TypeResolution_Checks.cpp` — outside
// this cycle's file ownership — filed as its own issue rather than fixed
// here; see that issue for the proposed CLASS-ancestor twin of
// `ifaceIndexerObligations`.
//
// THIS CYCLE'S FIX (applied to `classIndexerMatching`, LlvmBackend_impl.hpp):
// applies `indexerCandidateWireable` as a skip-and-continue filter, exactly
// matching `findIndexer`'s own #2437/#2442/#2449 pattern, so a closer-but-
// unwireable candidate no longer terminates the base-chain search before a
// farther WIREABLE candidate is found (the general shape the itable route
// already guards against). MEASURED: for the two-level shape this test
// pins, the fix changes NOTHING observable — built and ran the identical
// repro both WITH and WITHOUT the `classIndexerMatching` filter: SIGSEGV
// (exit 139) either way, because `importedVirtualAccessorSlotFn`'s own
// redundant abstract-modifier guard already produces the same null-slot
// outcome regardless of whether `classIndexerMatching` pre-filters. A
// "closer unwireable, farther wireable" shadowing shape (which the fix WOULD
// change) was traced but could not be constructed in valid Bezel:
// `Resolver::checkTypeStaticNotVirtual` (TypeResolution_Checks.cpp:474-499,
// reached via `checkStaticNotVirtual`) rejects `abstract override` outright
// — "member ... may have at most one of `abstract`, `virtual`, `override`,
// or `sealed override` — they are mutually exclusive (§12.1)" — and that
// combination is the only way to get a CLOSER match that is both slot-
// key-matching (an override) and unwireable (abstract) — a plain non-
// abstract override is always wireable (imported-but-concrete passes every
// `importedVirtualAccessorSlotFn` guard) and a non-overriding intermediate
// class contributes no candidate of its own for `classIndexerMatching`'s
// per-level `t->members` scan to find. (`checkClassModifierCombinations` /
// `checkTypeModifierCombinations`, TypeResolution_Checks.cpp:1781/:1789, are
// a DIFFERENT pair of functions checking OTHER modifier combinations, e.g.
// `closed` — an earlier revision of this comment misattributed the
// abstract/override check to them; corrected here.) The fix is still
// landed: it is a correctness/consistency improvement matching the itable
// route's own history, at zero measured cost, even though this cycle could
// not exhibit a case where it changes the outcome. A reviewer's stronger
// structural argument for the same conclusion: `indexerCandidateWireable`'s
// exclusions (abstract, interface-declared) are a STRICT SUBSET of the
// guards `importedVirtualAccessorSlotFn` already applies downstream, so
// applying the filter earlier can never cause `classIndexerMatching` to
// reject a candidate that would otherwise have resolved — the filter is
// pure hardening, never a new false-reject.
//
// THIS TEST pins the CURRENT (defective) sema-accept + runtime-crash
// behaviour as a KNOWN GAP, tied to the filed issue. It is expected to need
// updating (both compiles should instead report a §12.5-style diagnostic,
// and the executable should never be produced) once that issue's fix lands —
// per the project's "fix must update existing tests that pinned the old
// behaviour" discipline.

#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFileRow12 {
    std::string path;
    explicit TempFileRow12(std::string p) : path(std::move(p)) {}
    ~TempFileRow12() { std::remove(path.c_str()); }
};

void writeSourceRow12(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObjRow12(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

}  // namespace

// ===========================================================================
// A concrete class (`Derived`) inherits, without overriding, an IMPORTED
// abstract class's (`AB`) abstract indexer. Per §12.5 ("every concrete
// subclass must override [an abstract member]") this should be a compile
// error naming the missing override — the SAME diagnostic §12.5's own BFS
// already reports for an abstract METHOD or named PROPERTY left
// unimplemented. MEASURED: neither compile reports an error, and the linked
// executable SIGSEGVs (`d[5]` dispatches through `Derived`'s own vtable
// slot, left null because the only base-chain candidate is the unimplemented
// abstract ancestor's indexer).
// ===========================================================================
BEZEL_TEST(xmod_abstract_class_indexer_unoverridden_row12,
           unoverridden_imported_abstract_class_indexer_compiles_then_crashes) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bzrow12_");
    TempFileRow12 libSrc((dir / (stem + "_lib.bzl")).string());
    TempFileRow12 libObj((dir / (stem + "_lib.o")).string());
    writeSourceRow12(libSrc.path,
                     "module Row12Lib; "
                     "public abstract class AB "
                     "  public abstract property [i: int]: int; "
                     "end AB;");
    if (!compileToObjRow12(libSrc.path, libObj.path)) return;  // toolchain unavailable — skip
    if (!std::ifstream(libObj.path).good()) return;

    TempFileRow12 appSrc((dir / (stem + "_app.bzl")).string());
    TempFileRow12 appExe((dir / stem).string());
    writeSourceRow12(appSrc.path,
                     "module Row12App; import Row12Lib; "
                     "public class Derived: AB "
                     "end Derived; "
                     "public class C "
                     "  public static method Main(): int "
                     "    var d := new Derived(); "
                     "    return d[5]; "
                     "  end Main; "
                     "end C;");
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObj.path;
    d.objectPath = libObj.path;
    deps.push_back(d);
    // KNOWN GAP (pins the filed issue): this SUCCEEDS today. §12.5 says a
    // concrete subclass leaving an inherited abstract member unoverridden is
    // a compile error; that rule is not enforced for a CLASS-declared
    // abstract INDEXER (only the INTERFACE half is checked,
    // `ifaceIndexerObligations` in TypeResolution_Checks.cpp). Once the
    // filed sema fix lands, this expectation flips to `BEZEL_EXPECT_FALSE`
    // with a §12.5 diagnostic asserted on TEXT, and the run step below is
    // deleted (no executable is produced to run).
    bool compiled = bezel::test::compileFileForTest(
        driver, appSrc.path, appExe.path, deps).success;
    BEZEL_EXPECT_TRUE(compiled);
    if (!compiled) return;

    // Run the linked executable in a SEPARATE process (std::system, matching
    // the #2545 test family's own pattern) so a crash here cannot take down
    // this test binary.
    int status = std::system(appExe.path.c_str());
    BEZEL_EXPECT_TRUE(status != -1);

    // KNOWN GAP: the child must NOT complete successfully — but the SHAPE of
    // "did not complete successfully" is ORACLE-DEPENDENT, and this repo
    // gates on BOTH oracles, neither of which dominates (#2633). Under the
    // PLAIN build the child dies from the row-12 SIGSEGV directly:
    // WIFSIGNALED(status) with WTERMSIG(status) == SIGSEGV. Under `--full`
    // (BEZEL_AOT_FORCE_SANITIZE=address) ASan installs its own fault
    // handler: it intercepts the SEGV, prints its report, and terminates via
    // `_exit()` — a NORMAL exit (WIFEXITED), not a signal — conventionally
    // exit 1 (configurable via `ASAN_OPTIONS=exitcode=`), so WIFSIGNALED is
    // FALSE there. An earlier revision of this test asserted WIFSIGNALED
    // unconditionally and failed under `--full` (measured: deterministic,
    // 5/5 in the gate's arbitration re-run) — not sharding noise, an
    // oracle-shape mismatch. Accept EITHER shape of "did not complete
    // successfully": a signal-death by SIGSEGV, or an abnormal (non-zero)
    // WIFEXITED. A CLEAN exit 0 is the one outcome that must fail LOUDLY
    // rather than being folded into either accepted shape — it would mean
    // the row-12 crash silently stopped happening, which is exactly the
    // signal this pinned-gap row exists to catch (once #3641 lands,
    // `compiled` above becomes false and this whole block is deleted, not
    // relaxed further).
    const bool cleanExit = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    const bool crashedBySignal =
        WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
    const bool crashedUnderAsanExit =
        WIFEXITED(status) && WEXITSTATUS(status) != 0;
    BEZEL_EXPECT_FALSE(cleanExit);
    BEZEL_EXPECT_TRUE(crashedBySignal || crashedUnderAsanExit);
}

}  // namespace bezel::module
