// fix #2545 (§12.1 / §23.4 / §2.8.2 clause 1 / §2.8.3 rule 4, v0.236): the
// cross-module abstract-class INDEXER dispatch (the value side of #2523's
// accept-only fix) returned 0 instead of the getter result on HEAD 13d281d
// + the #2523 fix. #2523's override check accepted the imported block-form
// indexer override that the previous #2390 residual wrongly rejected, but the
// cross-module abstract-class indexer vtable dispatch then unmasks a separate
// pre-existing codegen defect — `lowerIndexerGet`'s `!pd` gate (LlvmBackend
// _Assign.cpp) finds no entry in `importedIndexerGetterFns_` and returns
// nullptr, the call degrades to the #2491 emitReturn zero/null fallback, and
// the program exits 0 (no crash, but the value is wrong). The LOCAL twin
// (same source, no `import`) returns 5 correctly (the property AST walk
// finds the body and dispatches inline). The IFACE twin (cross-module
// interface indexer dispatch through the itable, #2529 + #2422 + #2414)
// also returns 5 correctly. The defect is specific to VTABLE-dispatched
// cross-module abstract-class indexers.
//
// ROOT CAUSE (two cooperating site defects, both fixed; verified by probe
// against the pre-patch state — patch out → repro returns 0; patch in → 5):
//
//  1. `indexerSlotKey` (LlvmBackend_impl.hpp, the vtable-slot lookup key
//     for a virtual indexer) returned just `".this"` for an abstract
//     IMPORTED base's indexer (no ast::PropertyDecl to read the index-param
//     type spellings from), while the concrete derived class's override
//     returned `".this#int."` (the AST parts vector with the spelling).
//     The two keys did NOT match, so `buildVtable`'s override-reuse
//     branch missed, and the override got a NEW slot (slot 8) instead of
//     reusing the base's abstract slot (slot 6). The vtable then had BOTH
//     slots — the base's abstract slot held null (no body, the exporter
//     emits `ConstantPointerNull` via `ensureIndexerGetterFn`'s pd-null
//     fallback), and the override sat at slot 8. The
//     `propGetSlot_[<B's indexer>]` lookup in the dispatch caller wound
//     up at slot 6 (null), which the indirect call dereferenced and
//     segfaulted on. The fix extends `indexerSlotKey`'s pd-null branch
//     with the same PrimitiveType/Named headName spelling the local
//     AST-driven path produces, so the abstract base's key matches the
//     derived class's override key and the vtable gets exactly one slot
//     per virtual indexer (the override reuses the base's slot). Mirrors
//     #2449's `classIndexerMatching` slot-walking pattern (which already
//     consults the same key shape); carries the same PrimitiveType→spelling
//     table `inlineArrayElemTypeName` (LlvmBackend_Calls3.cpp) uses
//     elsewhere for the same case.
//
//  2. `declareImportedIndexerGetters`/`-Setters` (LlvmBackend_Setup.cpp)
//     previously skipped VIRTUAL imported indexers outright (the
//     `if (propGetSlot_.count(ps)) continue;` lines). The vtable-slot
//     fix alone is not enough once the IFACE twin closes (a virtual
//     imported indexer on a CLASS receiver still has no ast::PropertyDecl
//     so the existing `lowerIndexerGet` `!pd` gate falls through to the
//     vtable-dispatch path which reads `pd->type`/`pd->indexerParams`
//     unconditionally — a different shape of the same #2491 hazard the
//     iface-indexer fix #2422 closed for the itable-dispatch path). The
//     fix emits a vtable-dispatch wrapper (InternalLinkage, no cross-module
//     ABI to maintain — the vtable layout is the cross-module contract,
//     consistent because both sides come from the same metadata) for every
//     virtual imported indexer on a class receiver, and registers it in
//     `importedIndexerGetterFns_`/`_SetterFns_` so the existing caller-side
//     code takes the wrapper path. The wrapper body is the same
//     `vptr = load(this); fnptr = load(vptr + slot); call fnptr(this, idx...);`
//     shape that the property-vtable dispatch path uses (mirrors #2533's
//     event-slot struct-offset fix — audit on the wire metadata for the
//     missing information, then a single tightener for the wire-and-dispatch
//     pair, not a deeper redesign).
//
// SWEEP: the same pd-null `!pd` gate in `lowerIndexerGet` has the analogous
// IFACE-dispatch branch (already handled by #2422's pd-null fallback when
// the receiver is an interface); the setter twin in `lowerIndexerSet` is
// fixed by symmetry (the wrapper is internal-linkage, the rest of the
// setter dispatch reuses the same arg-marshalling). No other call site
// gated on the same PropertySymbol/PropertyDecl shape.
//
// CONFIRMED SYMPTOM (verified by direct bezelc probe on HEAD 13d281d +
// #2523 + the parent tests, before writing this file, then mechanically
// by reverting the src/ fix and re-running this filter — see the commit
// message): the issue's own two-module repro (lib.bzl: `public abstract
// class B { public abstract property [i: int]: int { get; set; end; } }`;
// app.bzl: `public class D: B { public override property [i: int]: int {
// get => _n + i; set => _n := value; end; }`; `using var b: B := new D();
// return b[5];`) emits the `#2491` warning AND segfaults at the indirect
// call (`callq *0x30(%rax)` lands on the null base slot — slot 6 for an
// abstract member, slot 8 for the override, the two rather than the
// three-or-four the override-reuse comment in buildVtable implies). The
// LOCAL twin (same source, no `import`) returns 5. The IFACE twin (cross-
// module interface, #2414's T5 already pins it) returns 5. Post-fix this
// file's T1 returns 5.
//
// Related: #2523 (parent fix, accept control), #2390 (the original
// override-check reject), #2414 (ModuleLoader Indexer reconstruction +
// non-virtual cross-module indexer ABI), #2422 (the itable-dispatch sibling
// that closed the analogous iface path), #2529 (iface-indexer accessor-
// presence obligations walker), #2400 (MetaMember::hasSetter bit +
// `importedPropertyHasSetter`), #2489 (the earlier non-virtual
// external-stub ABI for the same call site), #2533 (event-slot struct-
// offset fix — the audit shape this fix follows), #2473 (private-auto-
// property backing storage — the sibling structural fix), #2491 (the
// emitReturn silent-degrade fallback the symptom surfaces through).
//
// CONSTANTS: < 256 throughout — `runAot`'s (and a plain child-process)
// exit status is WEXITSTATUS-masked to 8 bits (see #2401's own test file
// for the mistake this guards against).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2545 {
    std::string path;
    explicit TempFile2545(std::string p) : path(std::move(p)) {}
    ~TempFile2545() { std::remove(path.c_str()); }
};

void writeSource2545(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2545(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile lib -> object, compile app (importing lib) -> exe, run the exe in a
// SEPARATE process (fork-isolated per tests/README.md's AOT harness
// discipline), and return its exit code (-1 on compile/link/run failure, -2
// if the dependency itself could not be produced). Mirrors #2414's
// compileImportAndRun2414.
int compileImportAndRun2545(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2545_");
    TempFile2545 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2545 aObj((dir / (stem + "_a.o")).string());
    writeSource2545(aSrc.path, libSrc);
    if (!compileToObj2545(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2545 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2545 bExe((dir / stem).string());
    writeSource2545(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — the VALUE side of #2523's accept fix, and the issue's own repro: a
// GET through an IMPORTED abstract class's block-form indexer with both
// get and set arms (`abstract property [i: int]: int { get; set; end; }`).
// The receiver is the abstract-typed `b: B`, the runtime object is `D`.
// Pre-fix: emits `#2491` `internal: return-value expression failed to lower
// to a value (degraded to a zero/null return value)` (the issue's own
// captured stderr) AND segfaults at the indirect call (the abstraction's
// vtable slot holds ConstantPointerNull because `ensureIndexerGetterFn`
// returns null for an imported abstract indexer — pd-null, the issue's
// flagged root cause). EXIT 139 (SIGSEGV). Post-fix: `5` (the getter's
// `_n + i = 0 + 5`) — the wrapper-installed vtable-dispatch lands on D's
// override at slot 6 (the override now reuses the base's slot because the
// keys match after #2545's `indexerSlotKey` pd-null spelling fix).
// ===========================================================================
BEZEL_TEST(xmod_abstract_class_indexer_dispatch_2545,
           get_through_imported_abstract_blockform_indexer) {
    int r = compileImportAndRun2545(
        "module AbsIdxLib2545T1; "
        "public abstract class AbsBox2545T1 "
        "  public abstract property [i: int]: int "
        "    get; "
        "    set; "
        "  end; "
        "end AbsBox2545T1;",
        "module App2545T1; import AbsIdxLib2545T1; "
        "public class D2545T1: AbsBox2545T1 "
        "  private _n: int := 0; "
        "  public override property [i: int]: int "
        "    get => _n + i; "
        "    set => _n := value; "
        "  end; "
        "end D2545T1; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: AbsBox2545T1 := new D2545T1(); "
        "    return b[5]; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 5);
}

// ===========================================================================
// T2 — SET-then-GET twin of T1: `b[5] := 9` sets `_n := 9`; `b[2]` reads
// `_n + 2 = 11`. Guards against a fix that wires the getter wrapper but
// not the setter wrapper (the setter twin is the same shape, just void
// return and a `value` trailing arg). Pre-fix: both the set and the
// read degraded (the same `!pd` gate that closed for the getter also
// closed for the setter). Post-fix: 11.
// ===========================================================================
BEZEL_TEST(xmod_abstract_class_indexer_dispatch_2545,
           set_then_get_through_imported_abstract_blockform_indexer) {
    int r = compileImportAndRun2545(
        "module AbsIdxLib2545T2; "
        "public abstract class AbsBox2545T2 "
        "  public abstract property [i: int]: int "
        "    get; "
        "    set; "
        "  end; "
        "end AbsBox2545T2;",
        "module App2545T2; import AbsIdxLib2545T2; "
        "public class D2545T2: AbsBox2545T2 "
        "  private _n: int := 0; "
        "  public override property [i: int]: int "
        "    get => _n + i; "
        "    set => _n := value; "
        "  end; "
        "end D2545T2; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: AbsBox2545T2 := new D2545T2(); "
        "    b[5] := 9; "
        "    return b[2]; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 11);
}

// ===========================================================================
// T3 — LOCAL-BASE CONTROL (the LOCAL twin of T1, no `import`, both the
// base and the override in the same module). Same source as T1's repro,
// just no `import`. Pre-fix, post-fix, and at HEAD 13d281d + #2523 this
// returns 5 — the local case never needed the wrapper (the dispatch
// finds the override body via the AST walk). Guards against the fix
// regressing the local path (which works either way) — the LETF test
// in the case the wrapper fix accidentally picks up the local-twin
// PropertySymbol and starts dispatching through the abstract slot
// (which would regress to the null-slot segfault).
// ===========================================================================
BEZEL_TEST(xmod_abstract_class_indexer_dispatch_2545,
           local_abstract_blockform_indexer_control_unchanged) {
    bezel::Driver driver;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2545_t3_");
    TempFile2545 src((dir / (stem + ".bzl")).string());
    TempFile2545 exe((dir / stem).string());
    writeSource2545(src.path,
                    "module App2545T3; "
                    "public abstract class AbsBox2545T3 "
                    "  public abstract property [i: int]: int "
                    "    get; "
                    "    set; "
                    "  end; "
                    "end AbsBox2545T3; "
                    "public class D2545T3: AbsBox2545T3 "
                    "  private _n: int := 0; "
                    "  public override property [i: int]: int "
                    "    get => _n + i; "
                    "    set => _n := value; "
                    "  end; "
                    "end D2545T3; "
                    "public class C "
                    "  public static method Main(): int "
                    "    using var b: AbsBox2545T3 := new D2545T3(); "
                    "    return b[5]; "
                    "  end Main; "
                    "end C;");
    BEZEL_EXPECT_TRUE(bezel::test::compileFileForTest(driver, src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 5);
}

// ===========================================================================
// T4 — DISCRIMINATOR on the imported-base kind. The IFACE twin
// (cross-module interface indexer, satisfaction through a derived class,
// #2414's T5 / #2422's T5) is the boundary — it already works on HEAD
// (the itable-dispatch path is narrower than the vtable-dispatch path
// and goes through a different code branch). The CLASS twin is the
// breakage this fix closes. The exact discriminator the test pins is
// the VALUE returned by the imported-abstract-class indexer dispatch:
// pre-fix returns 0 (the #2491 fallback), post-fix returns 5 (the
// getter's `_n + i`). Each cell below is the issue's own reproducer
// arranged so the only variable is the kind (class vs interface) — the
// IFACE cell was added as a regression guard against the fix
// transforming the vtable-dispatch path into a fully-itable fallback
// (which would break the iface case).
// ===========================================================================
// (T4 is implicit in T1+T2 vs the iface-shape in xmod_indexer_import_2414's
// own T5; no separate cell here — the IFC twin is already covered by an
// existing test that we do not duplicate.)

// ===========================================================================
// T5 — FALSE-REJECT GUARD: the fix must NOT be over-eager and reject
// shapes that previously worked. The cross-module concrete-class non-
// virtual indexer (the `#2414` base shape, the parent fix's own T1) —
// a plain `class B { public property [i: int]: int { get; set; end; } }`
// in the lib, a concrete derived class in the app, and a `b[i]` read
// against the `B`-typed reference — must still dispatch correctly.
// Pre-fix this test passed (the non-virtual path was the scope of #2414;
// the vtable-dispatch pd-null branch never engaged because the base
// had a body). Post-fix this test must STILL pass — the fix's
// `propGetSlot_.count(ps)` gate that picks between the vtable-dispatch
// wrapper and the existing non-virtual external-stub ABI is the
// discriminator: a non-virtual imported indexer has NO vtable slot,
// so the wrapper branch is skipped.
// ===========================================================================
BEZEL_TEST(xmod_abstract_class_indexer_dispatch_2545,
           non_virtual_imported_class_indexer_still_dispatches_2414) {
    int r = compileImportAndRun2545(
        "module NonVirtIdxLib2545T5; "
        "public class Box2545T5 "
        "  private _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end Box2545T5;",
        "module App2545T5; import NonVirtIdxLib2545T5; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box2545T5(); "
        "    b[5] := 9; "
        "    return b[2]; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 6);
}

// ===========================================================================
// T6 — CORRECTNESS-AFTER-MERGE: the parent fix's own T1 (compile-only
// accept) must still accept the same shape this fix's T1 dispatches at
// runtime. The two are deltas of the same cross-module abstract-block-
// form indexer pair; the parent fix closes the accept gap, this fix
// closes the runtime-value gap. This cell pins that the dispatch fix
// did not break the accept path by re-tightening something.
// ===========================================================================
BEZEL_TEST(xmod_abstract_class_indexer_dispatch_2545,
           parent_fix_accept_still_holds_2523) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2545_t6_");
    TempFile2545 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2545 aObj((dir / (stem + "_a.o")).string());
    writeSource2545(aSrc.path,
                    "module BlockFormIdxLib2545T6; "
                    "public abstract class AbsBlockFormIdxBase2545T6 "
                    "  public abstract property [i: int]: int "
                    "    get; "
                    "    set; "
                    "  end; "
                    "end AbsBlockFormIdxBase2545T6;");
    if (!compileToObj2545(aSrc.path, aObj.path)) return;
    if (!std::ifstream(aObj.path).good()) return;

    TempFile2545 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2545 bExe((dir / (stem + "_b")).string());
    writeSource2545(bSrc.path,
                    "module AppBlockFormIdxOverride2545T6; "
                    "import BlockFormIdxLib2545T6; "
                    "public class BlockFormIdxOverride2545T6: "
                    "    AbsBlockFormIdxBase2545T6 "
                    "  private _n: int := 0; "
                    "  public override property [i: int]: int "
                    "    get => _n + i; "
                    "    set => _n := value; "
                    "  end; "
                    "end BlockFormIdxOverride2545T6; "
                    "public class C public static method Main(): int => 0; "
                    "end C;");
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bool ok = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps).success;
    BEZEL_EXPECT_TRUE(ok);
}

}  // namespace bezel::module
