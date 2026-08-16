// fix #2421 (§24.7/§24.1/§24.3/§2.8.2 clause 1/§2.8.3 rule 4, v0.236):
// Subscribe/Raise on an IMPORTED event silently no-op'd even after #2407's
// symbol reconstruction. #2407 made an imported type's `event` member exist
// as a real EventSymbol (closing the "no symbol at all" gap and, as a side
// effect, correctly extending sema's §28.14 capture-admission check to the
// cross-module case), but that symbol reconstruction alone did NOT make
// cross-module Subscribe/Raise dispatch to a real backing EventSource<T>.
// FOUR separate `dynamic_cast<const ast::EventDecl*>(es->decl)`-gated
// consumers blocked it — an imported EventSymbol's `decl` is unconditionally
// null (no AST is reconstructed for an imported member, mirroring Property/
// Indexer's own #503/#2414 limitation), so all four silently produced
// nothing for the reconstructed event, exactly as before #2407:
//
//   1. the #2401 `ifaceEventIndex_` IMPORT-mirror (LlvmBackend_Setup.cpp,
//      declareVtables) needed `!ed->isAsync` to reserve an itable slot for
//      an imported interface's sync event — `eventSourcePtr`'s
//      `ifaceEventIndex_.find(es)` returned end() and the dispatch returned a
//      null source, so Subscribe silently registered on nothing.
//   2. the itab$C$I BUILD loop (Pass D, same file) needed the same `!ed->
//      isAsync` to append the per-class EventSource-accessor entry — even
//      with an index reserved, the itable array was built SHORTER than the
//      index expects (the GEP would read past the array).
//   3. `collectInstanceStorage` (LlvmBackend_impl.hpp) needed `!ed->isStatic`
//      to give a DIRECTLY-typed/imported-base event its per-instance backing-
//      field slot (fieldIndex_).
//   4. `eventPayloadType` (LlvmBackend_Calls2.cpp) needed `ed->type` to build
//      the handler closure's `(payload) -> ()` signature — a null payload
//      built the closure as `() -> ()`, so the lambda's payload parameter
//      (and any `e.V` body reference) failed to lower.
//
// THE FIX (this commit): add `isEventStatic`/`isEventAsync` metadata bits to
// `MetaMember` (mirroring #2400/#2461's hasSetter/hasGetter additive-scalar
// precedent exactly — emit/read/hashMember/operator==/copy-ctor/assign, the
// FIVE sites a new bit must stay consistent across), populate them from the
// local `ast::EventDecl` in ModuleMetadataBuilder, carry them across the
// boundary, and consult them in `eventIsStatic`/`eventIsAsync` helpers
// (decl-first, side-table fallback — the same shape `isAutoPropertyStorage`
// uses, LlvmBackend_impl.hpp) at the decl-null-gates. Plus a decl-null
// fallback in `eventPayloadType` (the imported-member-type side table, with a
// Unit type meaning "no payload" — matching the local `!ed->type` path).
//
// HAZARD NOTE (why a new bit, not a default): a WRONG guess on isAsync is a
// real hazard, not just an imprecision — it would reserve an importer-side
// itable slot the exporter's real itable layout does not have (or vice versa),
// an itable/struct-layout MISMATCH between the two compilations (the exact
// class of bug #2420/#2406 caught elsewhere). The wire bit is the sole safe
// source of truth for an imported event's classification. A pre-#2421 blob
// (no 15th/16th MEMBER_BLOCK scalar) defaults both bits to `false` — the
// classification a plain instance sync `event E: T;` (the common case)
// carries, so a stale not-yet-recompiled dependency keeps its already-broken
// pre-fix behavior rather than guessing.
//
// CONFIRMED SYMPTOM (probed live before writing this test): the issue's own
// repro (T1 below) compiled and ran clean but returned exit 100, not 110 —
// the subscribed lambda never fired. POST-fix it returns 110 (acc.V rises by
// e.V*10 on Raise). All expected constants are kept < 256 — exit status is
// WEXITSTATUS-masked to 8 bits.

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
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2421 {
    std::string path;
    explicit TempFile2421(std::string p) : path(std::move(p)) {}
    ~TempFile2421() { std::remove(path.c_str()); }
};

void writeSource2421(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib -> object, compile app (importing lib) -> exe, run it. Returns
// {depOk, appCompileOk, runExit} (runExit is WEXITSTATUS, or -1 on run
// failure / -2 if the dependency could not be produced).
struct Result2421 {
    bool depOk = false;
    bool appCompileOk = false;
    int runExit = -1;
};

Result2421 compileImportAndRun2421(const std::string& libSrc,
                                   const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2421_");
    TempFile2421 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2421 aObj((dir / (stem + "_a.o")).string());
    writeSource2421(aSrc.path, libSrc);
    Result2421 r;
    bezel::Driver driver;
    r.depOk = driver.compileObject(aSrc.path, aObj.path).success &&
              std::ifstream(aObj.path).good();
    if (!r.depOk) return r;

    TempFile2421 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2421 bExe((dir / stem).string());
    writeSource2421(bSrc.path, appSrc);
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

// A library exporting an INTERFACE with a sync payload event, the payload
// record, and (for the no-payload axis) a unit-payload interface event.
const char* kLib =
    "module EvtLib2421; "
    "public record EvType2421(V: int); "
    "public interface ISource2421 "
    "  event Ready: EvType2421; "
    "end ISource2421; "
    "public interface ISignal2421 "
    "  event Ping; "
    "end ISignal2421;";

}  // namespace

// ===========================================================================
// T1 — the issue's own repro (PRIMARY regression). PRE-fix this returned 100
// (the subscribed lambda never fired — eventSourcePtr's ifaceEventIndex_
// lookup returned end() for the imported event, so Subscribe registered on a
// null source). POST-fix it returns 110 (acc.V rises by e.V*10 = 10 on Raise,
// then +100). `using var i` keeps the subscribe well-formed per §28.14 (the
// bounding object is a using-var local), so the program is NOT ill-formed —
// the bug was purely the silent dispatch no-op.
// ===========================================================================
BEZEL_TEST(xmod_event_subscribe_import_2421,
           subscribe_then_raise_through_imported_interface_fires) {
    auto r = compileImportAndRun2421(
        kLib,
        "module App2421T1; import EvtLib2421; "
        "public class Source2421T1: ISource2421 "
        "  public event Ready: EvType2421; "
        "  public method Trigger(v: int) Ready.Raise(new EvType2421(v)); "
        "  end Trigger; "
        "end Source2421T1; "
        "public class Acc2421T1 "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc2421T1; "
        "public class C "
        "  public static method Main(): int "
        "    using var acc := new Acc2421T1(); "
        "    using var i: ISource2421 := new Source2421T1(); "
        "    var sub := i.Ready.Subscribe(lambda(e) => do "
        "                 acc.Add(e.V * 10); end); "
        "    var src: Source2421T1 := (i as Source2421T1)!; "
        "    src.Trigger(1); "
        "    return acc.V + 100; "
        "  end Main; "
        "end C;");
    if (!r.depOk) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(r.appCompileOk);
    BEZEL_EXPECT_EQ(r.runExit, 110);  // handler fires: acc.V=10, +100
}

// ===========================================================================
// T2 — MULTIPLE subscribers through the imported interface both fire (§24.5:
// "invoke every active handler closure ... in subscription order"). PRE-fix
// NEITHER fired; POST-fix both fire, so acc.V rises by 10 + 20 = 30 (+100 =
// 130). Pins that the cross-module dispatch reaches the SAME backing
// EventSource for every subscriber (the itable accessor returns the real
// per-instance field) and that handler order is preserved.
// ===========================================================================
BEZEL_TEST(xmod_event_subscribe_import_2421,
           multiple_subscribers_through_imported_interface_both_fire) {
    auto r = compileImportAndRun2421(
        kLib,
        "module App2421T2; import EvtLib2421; "
        "public class Source2421T2: ISource2421 "
        "  public event Ready: EvType2421; "
        "  public method Trigger(v: int) Ready.Raise(new EvType2421(v)); "
        "  end Trigger; "
        "end Source2421T2; "
        "public class Acc2421T2 "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc2421T2; "
        "public class C "
        "  public static method Main(): int "
        "    using var acc := new Acc2421T2(); "
        "    using var i: ISource2421 := new Source2421T2(); "
        "    var s1 := i.Ready.Subscribe(lambda(e) => do "
        "                 acc.Add(e.V * 10); end); "
        "    var s2 := i.Ready.Subscribe(lambda(e) => do "
        "                 acc.Add(e.V * 20); end); "
        "    var src: Source2421T2 := (i as Source2421T2)!; "
        "    src.Trigger(1); "
        "    return acc.V + 100; "
        "  end Main; "
        "end C;");
    if (!r.depOk) return;
    BEZEL_EXPECT_TRUE(r.appCompileOk);
    BEZEL_EXPECT_EQ(r.runExit, 130);  // both fire: 10 + 20, +100
}

// ===========================================================================
// T3 — a NO-PAYLOAD (unit) event on an imported interface (§24: `event Ping;`
// with no `: T`). Pins the eventPayloadType Unit branch: the exporter encodes
// the omitted `: T` as a Unit MetaType, and the importer's fallback must read
// it as "no payload" (null), building the handler as `() -> ()` — the same
// shape the local `!ed->type` path always produced. PRE-fix this returned 100
// (handler never fired); POST-fix 107 (acc.V rises by 7, +100).
// ===========================================================================
BEZEL_TEST(xmod_event_subscribe_import_2421,
           no_payload_imported_interface_event_fires) {
    auto r = compileImportAndRun2421(
        kLib,
        "module App2421T3; import EvtLib2421; "
        "public class Signaller2421T3: ISignal2421 "
        "  public event Ping; "
        "  public method Go() Ping.Raise(); end Go; "
        "end Signaller2421T3; "
        "public class Acc2421T3 "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc2421T3; "
        "public class C "
        "  public static method Main(): int "
        "    using var acc := new Acc2421T3(); "
        "    using var s: ISignal2421 := new Signaller2421T3(); "
        "    var sub := s.Ping.Subscribe(lambda() => do "
        "                 acc.Add(7); end); "
        "    var sig: Signaller2421T3 := (s as Signaller2421T3)!; "
        "    sig.Go(); "
        "    return acc.V + 100; "
        "  end Main; "
        "end C;");
    if (!r.depOk) return;
    BEZEL_EXPECT_TRUE(r.appCompileOk);
    BEZEL_EXPECT_EQ(r.runExit, 107);  // unit handler fires: acc.V=7, +100
}

}  // namespace bezel::module
