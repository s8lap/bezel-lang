// fix #2407 (§11.2/§24/§2.8.2 clause 1/§2.8.3 rule 4, v0.235): ModuleLoader::
// convertMember had NO `case MetaMemberKind::Event:` — the comment there
// read "Indexer / Event are part of the schema but deferred." An imported
// type's `event` member was simply absent from the reconstructed
// TypeSymbol::members list: not merely un-indexed (#2401's own filed root
// cause for properties/events reached through an imported interface — that
// fix's ifaceEventIndex_ mirror extension had nothing to iterate over for
// events specifically, since no EventSymbol existed at all), but never a
// symbol in the first place.
//
// CONFIRMED SYMPTOM (probed live before writing this test, exactly as the
// issue's own repro describes): `i.Ready.Subscribe(lambda(e) => ...)`
// through a reference typed by an IMPORTED interface compiled and ran
// clean (no diagnostic, no crash) but the subscribe never reached the real
// backing EventSource<T> — verified via the encoded exit code (100, not the
// expected 110: the lambda never runs).
//
// A SECOND, independently-confirmed effect of the missing symbol, found
// while probing this fix (T1 below): with no EventSymbol to resolve
// `i.Ready` to, sema's §28.14 capture-admission check for an event-
// subscribe expression NEVER RAN for the cross-module case — silently
// admitting a capture-bearing lambda through a non-`using var` interface
// local, a shape that already correctly REJECTS today for a fully LOCAL
// (no-import) program with the IDENTICAL structure (verified: reverting
// this fix and compiling the local-only analog of T1's source produces the
// SAME §28.14 diagnostic this fix now produces for the cross-module case).
// This fix's symbol reconstruction closes that consistency gap as a direct
// side effect (not a separate change) — the cross-module case reaches the
// SAME §28.14 check the local case always did, because it now has a real
// EventSymbol to resolve `i.Ready` to, exactly as the local case always had.
//
// THE FIX (this commit): add `case MetaMemberKind::Event:` to
// ModuleLoader.cpp's convertMember, reconstructing a real EventSymbol
// (mirroring the shape of the Property/Indexer cases — no ast::EventDecl to
// attach, since there is no AST for an imported member; the payload TYPE
// rides the existing fieldTypes_ side table Property/Indexer already use).
//
// SCOPE / WHAT REMAINS BROKEN (probed, NOT fixed here — filed as #2421):
// reconstructing the symbol makes it VISIBLE to sema (T1's rejection proves
// this) but does NOT make Subscribe/Raise dispatch to a real backing store.
// TWO further `ed = dynamic_cast<ast::EventDecl*>(es->decl)`-gated
// consumers block that, both unconditionally null for an imported
// EventSymbol and both requiring a NEW metadata bit (isAsync and/or
// isStatic) this fix does not add (a wrong guess would risk an itable/
// struct-layout mismatch between exporter and importer — the same class of
// hazard #2420 caught elsewhere in this session, via a DIFFERENT mechanism):
//   1. collectInstanceStorage (LlvmBackend_impl.hpp) — needs `ed` to even
//      add the event's backing-field slot to instance storage, so the
//      per-instance struct offset for a DIRECTLY-typed imported event still
//      resolves to nothing (fieldIndex_ has no entry).
//   2. the #2401 ifaceEventIndex_ mirror (LlvmBackend_Setup.cpp,
//      declareVtables) — needs `ed`/`!ed->isAsync` to reserve an itable
//      slot for an INTERFACE-typed imported event.
// T2 below is the accept-control proving this (compiles+runs clean, value
// NOT asserted — see the file's own account and #2421), mirroring the
// project's established #2401 T5 / #2414 T4 deferral pattern.
//
// Also unresolved (flagged, not decided here, per CLAUDE.md rule 4): #2407's
// own issue text notes the spec RETIRES the `event` member kind entirely
// (§24, v0.235: "the word `event` remains reserved... an `event`
// declaration is a reserved-word error") while the parser still accepts it.
// Whether the long-term fix is completing this legacy surface (what this
// commit does, matching #2401's own already-landed direction) or
// implementing the retirement is a design question left open.
//
// CONSTANTS: kept < 256 (T2's control constant included) — see #2401's own
// test file for the exact mistake this discipline guards against
// (WEXITSTATUS masks to 8 bits).

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

struct TempFile2407 {
    std::string path;
    explicit TempFile2407(std::string p) : path(std::move(p)) {}
    ~TempFile2407() { std::remove(path.c_str()); }
};

void writeSource2407(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2407(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile lib -> object, compile app (importing lib) -> exe. Returns the
// exe's exit code (-1 on run failure), or, when `expectCompileFail` is
// true, treats a clean APP compile as the failure (returns -3) and a
// rejected APP compile as success (returns 0) — used by the reject-shape
// test (T1) where the compile itself is the observable. -2 if the
// dependency itself could not be produced.
struct CompileImportResult2407 {
    bool depOk = false;
    bool appCompileOk = false;
    int runExit = -1;
};

CompileImportResult2407 compileImportAndRun2407(const std::string& libSrc,
                                                 const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2407_");
    TempFile2407 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2407 aObj((dir / (stem + "_a.o")).string());
    writeSource2407(aSrc.path, libSrc);
    CompileImportResult2407 r;
    r.depOk = compileToObj2407(aSrc.path, aObj.path) &&
              std::ifstream(aObj.path).good();
    if (!r.depOk) return r;

    TempFile2407 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2407 bExe((dir / stem).string());
    writeSource2407(bSrc.path, appSrc);
    bezel::Driver driver;
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

const char* kLib =
    "module EvtLib2407; "
    "public record EvType2407(V: int); "
    "public interface ISource2407 "
    "  event Ready: EvType2407; "
    "end ISource2407;";

}  // namespace

// ===========================================================================
// T1 — the issue's own repro shape (`var i`/`var src`, not `using var`):
// PRE-fix this compiled and ran clean, returning 100 (the subscribe silently
// had no effect — no EventSymbol existed to resolve `i.Ready` to at all, so
// §28.14's capture-admission check for an event subscribe never ran for the
// cross-module case). POST-fix, `i.Ready` resolves to a real (reconstructed)
// EventSymbol, §28.14 correctly runs, and the program is correctly REJECTED
// for the same reason the byte-identical LOCAL-only analog already was
// (verified separately, not asserted here — see the file header): the
// subscribe's bounding object `i` is a plain var, not a `using var`/`this`.
// This is a genuine soundness/consistency improvement, not merely "still
// broken" — the program was arguably ill-formed all along; the bug was that
// the cross-module path silently accepted it while the local path did not.
// ===========================================================================
BEZEL_TEST(xmod_event_import_2407,
           subscribe_through_imported_interface_ill_formed_now_rejected) {
    auto r = compileImportAndRun2407(
        kLib,
        "module App2407T1; import EvtLib2407; "
        "public class Source2407T1: ISource2407 "
        "  public event Ready: EvType2407; "
        "  public method Trigger(v: int) Ready.Raise(new EvType2407(v)); "
        "  end Trigger; "
        "end Source2407T1; "
        "public class Acc2407T1 "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc2407T1; "
        "public class C "
        "  public static method Main(): int "
        "    var acc := new Acc2407T1(); "
        "    var src := new Source2407T1(); "
        "    var i: ISource2407 := src; "
        "    var sub := i.Ready.Subscribe(lambda(e) => do "
        "                 acc.Add(e.V * 10); end); "
        "    src.Trigger(1); "
        "    return acc.V + 100; "
        "  end Main; "
        "end C;");
    if (!r.depOk) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_FALSE(r.appCompileOk);
}

// ===========================================================================
// T2 — ACCEPT-CONTROL (compiles+runs clean — mirrors #2401 T5 / #2414 T4's
// documented deferral pattern): the SAME shape, corrected to be well-formed
// per §28.14 (`using var i: ISource2407 := new Source2407T1();`, so the
// subscribe's bounding object IS a `using var` local). Does NOT assert the
// runtime value: it currently still returns 100 (the lambda never runs) --
// see the file header's "SCOPE" note for the two separate, unfixed
// decl-null gates (collectInstanceStorage's field-slot allocation and
// #2401's ifaceEventIndex_ itable mirror) that block the VALUE from being
// correct, filed as #2421 rather than fixed here.
// ===========================================================================
BEZEL_TEST(xmod_event_import_2407,
           subscribe_through_imported_interface_well_formed_still_compiles_accept_control) {
    auto r = compileImportAndRun2407(
        kLib,
        "module App2407T2; import EvtLib2407; "
        "public class Source2407T2: ISource2407 "
        "  public event Ready: EvType2407; "
        "  public method Trigger(v: int) Ready.Raise(new EvType2407(v)); "
        "  end Trigger; "
        "end Source2407T2; "
        "public class Acc2407T2 "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Acc2407T2; "
        "public class C "
        "  public static method Main(): int "
        "    using var acc := new Acc2407T2(); "
        "    using var i: ISource2407 := new Source2407T2(); "
        "    var sub := i.Ready.Subscribe(lambda(e) => do "
        "                 acc.Add(e.V * 10); end); "
        "    var src: Source2407T2 := (i as Source2407T2)!; "
        "    src.Trigger(1); "
        "    return acc.V + 100; "
        "  end Main; "
        "end C;");
    if (!r.depOk) return;
    BEZEL_EXPECT_TRUE(r.appCompileOk);
}

// ===========================================================================
// T3 — CONTROL: the well-formed shape (T2's structure), fully LOCAL (no
// import at all). Confirms the fix does not disturb ordinary same-module
// event subscribe/raise, and pins the CORRECT value (110) that T2's
// cross-module analog does not yet reach — the gap T2 documents is
// specifically a cross-module ABI gap, not a general regression in event
// semantics.
// ===========================================================================
BEZEL_TEST(xmod_event_import_2407, local_interface_event_control_unaffected) {
    bezel::Driver driver;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2407_t3_");
    TempFile2407 src((dir / (stem + ".bzl")).string());
    TempFile2407 exe((dir / stem).string());
    writeSource2407(src.path,
                    "module LocalCtl2407T3; "
                    "public record EvTypeL2407T3(V: int); "
                    "public interface ISourceL2407T3 "
                    "  event Ready: EvTypeL2407T3; "
                    "end ISourceL2407T3; "
                    "public class SourceL2407T3: ISourceL2407T3 "
                    "  public event Ready: EvTypeL2407T3; "
                    "  public method Trigger(v: int) "
                    "    Ready.Raise(new EvTypeL2407T3(v)); "
                    "  end Trigger; "
                    "end SourceL2407T3; "
                    "public class AccL2407T3 "
                    "  public V: int := 0; "
                    "  public method Add(x: int) V := V + x; end Add; "
                    "end AccL2407T3; "
                    "public class C "
                    "  public static method Main(): int "
                    "    using var acc := new AccL2407T3(); "
                    "    using var i: ISourceL2407T3 := new SourceL2407T3(); "
                    "    var sub := i.Ready.Subscribe(lambda(e) => do "
                    "                 acc.Add(e.V * 10); end); "
                    "    var src: SourceL2407T3 := (i as SourceL2407T3)!; "
                    "    src.Trigger(1); "
                    "    return acc.V + 100; "
                    "  end Main; "
                    "end C;");
    BEZEL_EXPECT_TRUE(driver.compileFile(src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 110);
}

}  // namespace bezel::module
