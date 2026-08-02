// fix #2533 (§24.1/§22.8/§2.8.2 clause 1, v0.236): the importer's struct
// layout for an event declared on an IMPORTED base class mismatched the
// exporter's, because `Emitter::storageType` (LlvmBackend_Layout.cpp)
// returned the wire-carried PAYLOAD type for an `EventSymbol` whose `decl`
// is null (i.e. an imported event) — the event's `: T` payload (e.g.
// `EvB2533`) is in the wire as MetaMember::type, but the EVENT'S BACKING
// STORAGE is a runtime `EventSource<T>` referenced by an opaque pointer
// (§24.1), not the payload type. With the payload type in the event slot,
// the importer's `classStruct(Derived2533)` laid it out as `{ ptr, i8,
// %EvB2533 }` (where %EvB2533 is the payload record `{ i32 }`) — the
// EventSource pointer landed at offset 12 (the record's struct slot, the
// third field, offset 12 because the record's alignment is 4). The
// exporter's `bz$<Base>$init` (called via `emitImportedBaseFieldInit`)
// writes the EventSource at offset 16 per the exporter's own struct
// (`{ ptr, i8, ptr }` with alignment 8, event at offset 16). When the
// importer's itable accessor `bz$Derived2533$Ready$evtget` reads at offset
// 12, it gets the lower 4 bytes of the EventSource ptr (or other garbage
// over the field boundary), hands the bogus pointer to `bz_event_subscribe`
// → `bz_event_add`, which dereferences a high invalid address and SEGVs
// (exit 139, `The signal is caused by a READ memory access ... a high
// value address` per ASan).
//
// #2421 closed the IMPORTED-INTERFACE event shape (the corresponding
// declared-on-interface event) but the IMPORTED-BASE shape — an event
// inherited from an imported class, subscribed through a local interface —
// was the remaining gap. The fix is to take the EventSymbol-fast-path
// (`return ptrTy()`) BEFORE the imported-member-type fallback (and the
// decl-dependent fd/pd branches); the storage TYPE is always `ptr` for an
// event regardless of import status or `isEventStatic`/`isEventAsync`.
// The classification gate on isStatic (which determines whether the slot
// is INITIALIZED at all) sits in `collectInstanceStorage` / `emitOwnFieldInit`
// behind the `eventIsStatic` helper, unchanged.
//
// CONFIRMED SYMPTOM (probed live before writing this test): the issue's
// own two-module repro (libbase.bzl exports a class with an event,
// appbase.bzl imports + locally subclasses + subscribes through a local
// interface) compiled clean but SEGV'd (exit 139). Mechanical
// fail-without-fix demonstration: patch out the `EventSymbol` fast-path
// again (i.e. revert) → SEGV reproduced. PASS-with-fix: handler fires
// (acc.V rises by 3 on Raise), +100 = exit 103.
//
// All expected constants kept < 256 — exit status is WEXITSTATUS-masked to
// 8 bits.

#include <cstdio>
#include <cstdlib>
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

struct TempFile2533 {
    std::string path;
    explicit TempFile2533(std::string p) : path(std::move(p)) {}
    ~TempFile2533() { std::remove(path.c_str()); }
};

void writeSource2533(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib -> object, compile app (importing lib) -> exe, run it. Returns
// {depOk, appCompileOk, runExit} (runExit is WEXITSTATUS, or -1 on run
// failure / -2 if the dependency could not be produced).
struct Result2533 {
    bool depOk = false;
    bool appCompileOk = false;
    int runExit = -1;
};

Result2533 compileImportAndRun2533(const std::string& libSrc,
                                   const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2533_" + std::to_string(std::rand());
    TempFile2533 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2533 aObj((dir / (stem + "_a.o")).string());
    writeSource2533(aSrc.path, libSrc);
    Result2533 r;
    bezel::Driver driver;
    r.depOk = driver.compileObject(aSrc.path, aObj.path).success &&
              std::ifstream(aObj.path).good();
    if (!r.depOk) return r;

    TempFile2533 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2533 bExe((dir / stem).string());
    writeSource2533(bSrc.path, appSrc);
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    r.appCompileOk = driver.compileFile(bSrc.path, bExe.path, deps).success;
    if (!r.appCompileOk) return r;
    int status = std::system(bExe.path.c_str());
    r.runExit = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return r;
}

// The library exports a class with a sync payload event (an instance event
// inherited by an importer-side subclass). The event is `public`, so the
// exporter carries it through the metadata wire as a public EventSymbol.
const char* kLib2533 =
    "module EvtBaseLib2533; "
    "public record EvB2533(V: int); "
    "public class BaseWithEvt2533 "
    "  public event Ready: EvB2533; "
    "  public method Trigger(v: int) Ready.Raise(new EvB2533(v)); "
    "  end Trigger; "
    "end BaseWithEvt2533;";

}  // namespace

// ===========================================================================
// T1 — the issue's own repro. PRE-fix this segfaults (exit 139, the
// `bz_event_add` SEGV from a garbage EventSource pointer) because the
// importer's `emitEventAccessor(Derived2533, "Ready")` reads the wrong
// byte of the object. POST-fix the handler fires (acc.V rises by e.V*3
// = 3 on Raise), +100 = exit 103.
// ===========================================================================
BEZEL_TEST(xmod_imported_base_event_2533,
           subscribe_then_raise_through_imported_base_event_fires) {
    auto r = compileImportAndRun2533(
        kLib2533,
        "module AppBase2533; import EvtBaseLib2533; "
        "public interface IReady2533 "
        "  event Ready: EvB2533; "
        "end IReady2533; "
        "public class Derived2533: BaseWithEvt2533, IReady2533 end Derived2533; "
        "public class Acc2533B "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc2533B; "
        "public class C "
        "  public static method Main(): int "
        "    using var acc := new Acc2533B(); "
        "    using var i: IReady2533 := new Derived2533(); "
        "    var sub := i.Ready.Subscribe(lambda(e) => do "
        "                 acc.Add(e.V * 3); end); "
        "    var d: Derived2533 := (i as Derived2533)!; "
        "    d.Trigger(1); "
        "    return acc.V + 100; "
        "  end Main; "
        "end C;");
    if (!r.depOk) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(r.appCompileOk);
    BEZEL_EXPECT_EQ(r.runExit, 103);  // handler fires: acc.V=3, +100
}

}  // namespace bezel::module
