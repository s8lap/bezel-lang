// Regression tests for #4553 — §2.8.2 / §14.2 / §14.1 (v0.306): a §14.2 `where`
// constraint must survive the artifact boundary, so an imported generic's body
// may call the member the constraint provides.
//
// THE DEFECT (measured on origin/main b02083f3, the rule-corpus case
// `s2_8_2_0005`). `ModuleLoader`'s §14.1/#783 importer-side monomorphization
// re-parses an imported generic class's exported method body inside a SYNTHETIC
// single-type source (`buildSyntheticMethodSource`). That source rendered the
// class header's generic parameter LIST but DROPPED its `where` clauses:
//
//     public class Holder<T> where T: Base     (the real declaration)
//     public class Holder<T>                   (what was re-parsed)
//
// `T` is then unconstrained in the fresh context, so a body that calls a
// constraint-provided member — `public method Describe(item: T): string =>
// item.Tag();` — does not type-resolve, the #783 soundness gate declines to
// attach the decl, and `ms->decl` stays null. Codegen's HARD route in
// `emitMethodCall` requires that decl; without it the call fell through to the
// DEFINING module's i32-PLACEHOLDER template and emitted
//
//     %13 = load ptr, ptr %1, align 8
//     %14 = call ptr @"bz$Lib.Holder$Describe"(ptr %12, ptr %13)
//
// against a `declare ptr @"bz$Lib.Holder$Describe"(ptr, i32)` — a `ptr` argument
// in an `i32` parameter position, which LLVM's own verifier rejects:
// "Call parameter type does not match function signature!". The importing
// compile therefore FAILS; the same program written as ONE module compiles and
// prints `base`, which is precisely the §2.8.2 divergence:
//
//   §2.8.2 — "The artifact interface must be sufficient for a consumer to
//   instantiate every public generic type at arbitrary module-external type
//   arguments satisfying the declared constraints; an external instantiation
//   behaves identically to a same-module instantiation at the same arguments."
//
// An IR-verifier failure is the loud form of a miscompile: the same disagreeing
// signature silently passes a 64-bit pointer through a 32-bit parameter wherever
// the verifier happens not to object.
//
// THE FIX (this commit). `buildSyntheticMethodSource` renders the `where`
// clauses (A.3 `GenericConstraints*`), and `parseAndAttachMethodBody` registers
// the exporting module's sibling `TypeSymbol`s that those clauses NAME into the
// fresh context (plus the `import <Module>;` line that lets the name bind), so
// `where T: Base` resolves to the REAL imported `Base`. A call to a member the
// constraint provides is then carried into the main context by identity — it is
// already an imported symbol, so there is nothing to remap.
//
// DELIBERATELY NARROW. Only the CONSTRAINT types are registered, never every
// public sibling: a constraint is part of the generic's own public signature
// (§14.2) and already rides the wire, whereas a body reaching an ARBITRARY
// sibling is the shape `xmod_generic_method_783`'s T5 canary keeps gated (it
// cannot be faithfully reconstructed, and a silently-dropped call returns a
// wrong value). Row 4 below pins that the canary still rejects. The rendering is
// all-or-nothing — a constraint this reconstruction cannot NAME (a builtin
// interface, an unexported `internal` sibling, a constructed `IFoo<U>`) makes
// the whole clause unrendered, i.e. exactly the pre-fix behaviour, which row 3
// pins for the previously-working case.
//
// THE TEST. Every row crosses a REAL `--import` boundary; an in-module test
// cannot pin this bug, because the defect is in the importer's reconstruction of
// an exported body. The AOT harness (`runAot`) compiles ONE module and cannot
// express a dependency, so these rows hand-roll `Driver::compileObject` for the
// library plus `bezel::test::compileFileForTest` for the importer (the
// established two-module shape — tests/module/xmod_checked_mode_forwarding_4488
// _test.cpp, tests/module/xmod_generic_method_783_test.cpp) and run the product
// through `bezel::test::runProgramForTest` rather than `std::system`, so the
// ambient LeakSanitizer of a `gate.sh --full` sweep cannot replace the program's
// own exit status (#4532). Row 1 also compiles the SINGLE-MODULE twin through
// `runAot`, because "identical to a same-module instantiation" is the §2.8.2
// clause the defect broke and an equivalence is only an equivalence when both
// halves are measured. No JIT oracle is used.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile4553 {
    std::string path;
    explicit TempFile4553(std::string p) : path(std::move(p)) {}
    ~TempFile4553() { std::remove(path.c_str()); }
};

void writeSource4553(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Build `libSrc` as a `-c` library object, then build `appSrc` as an executable
// importing it, run it, and return the program's exit code.
//
// Distinct negative sentinels, so a broken scaffold can NEVER read as a pass —
// every runnable row below asserts an exact non-negative value:
//   -2  the library half did not build (or produced no object)
//   -3  the importer half did not build/link  <-- the PRE-FIX result of row 1/2
//   -4  the program could not be run to a normal exit
int buildImportAndRun4553(const std::string& libSrc, const std::string& appSrc,
                          const char* tag) {
    const fs::path& dir = wtScratchDir(tag);
    const std::string id = unique_stem(tag);
    TempFile4553 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile4553 libObjF((dir / (id + "_lib.o")).string());
    TempFile4553 appSrcF((dir / (id + "_app.bzl")).string());
    TempFile4553 appExeF((dir / id).string());
    writeSource4553(libSrcF.path, libSrc);
    writeSource4553(appSrcF.path, appSrc);

    bezel::Driver driver;
    if (!driver.compileObject(libSrcF.path, libObjF.path).success) return -2;
    if (!std::ifstream(libObjF.path).good()) return -2;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);

    codegen::CodegenOptions appOpts;
    // compileFileForTest, not compileFile, so `gate.sh --full`'s
    // BEZEL_AOT_FORCE_SANITIZE sweep reaches the importer half (#3653).
    if (!bezel::test::compileFileForTest(driver, appSrcF.path, appExeF.path,
                                         deps, appOpts)
             .success)
        return -3;

    int status = bezel::test::runProgramForTest(appExeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -4;
}

// True when the importing half FAILS to compile (the gate canary, row 4).
bool buildImportFails4553(const std::string& libSrc, const std::string& appSrc,
                          const char* tag) {
    const fs::path& dir = wtScratchDir(tag);
    const std::string id = unique_stem(tag);
    TempFile4553 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile4553 libObjF((dir / (id + "_lib.o")).string());
    TempFile4553 appSrcF((dir / (id + "_app.bzl")).string());
    TempFile4553 appExeF((dir / id).string());
    writeSource4553(libSrcF.path, libSrc);
    writeSource4553(appSrcF.path, appSrc);

    bezel::Driver driver;
    // A library half that does not build would make this helper return true for
    // the wrong reason, so it is asserted by the caller through this flag.
    if (!driver.compileObject(libSrcF.path, libObjF.path).success) return false;
    if (!std::ifstream(libObjF.path).good()) return false;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);

    codegen::CodegenOptions appOpts;
    return !bezel::test::compileFileForTest(driver, appSrcF.path, appExeF.path,
                                            deps, appOpts)
                .success;
}

// ── Row 1 sources — the rule-corpus case s2_8_2_0005, verbatim in shape ──────
// `Describe`'s body calls `Tag`, which `T` has ONLY because of `where T: Base`.
// `Local` is declared by the CONSUMER, so the instantiation cannot have been
// seen by the defining module — §2.8.2's "arbitrary module-external type
// arguments satisfying the declared constraints".
const char* kLib4553 =
    "module CmLib4553; "
    "public class CmBase4553 "
    "  public constructor() end constructor; "
    "  public method Tag(): string => \"base\"; "
    "end CmBase4553; "
    "public class CmHolder4553<T> where T: CmBase4553 "
    "  public constructor() end constructor; "
    "  public method Describe(item: T): string => item.Tag(); "
    "end CmHolder4553; ";

const char* kApp4553 =
    "module CmApp4553; "
    "import CmLib4553; "
    "public class CmLocal4553: CmBase4553 "
    "  public constructor() end constructor; "
    "end CmLocal4553; "
    "public class CmMain4553 "
    "  public static method Main(): int "
    "    using var item := new CmLocal4553(); "
    "    using var holder := new CmHolder4553<CmLocal4553>(); "
    "    return holder.Describe(item).Length; "
    "  end Main; "
    "end CmMain4553; ";

// The SINGLE-MODULE twin of row 1: the same declarations and the same `Main`
// in one compilation unit. §2.8.2 requires the two to answer identically.
const char* kSingleModuleTwin4553 =
    "module CmOne4553; "
    "public class CmBase4553 "
    "  public constructor() end constructor; "
    "  public method Tag(): string => \"base\"; "
    "end CmBase4553; "
    "public class CmHolder4553<T> where T: CmBase4553 "
    "  public constructor() end constructor; "
    "  public method Describe(item: T): string => item.Tag(); "
    "end CmHolder4553; "
    "public class CmLocal4553: CmBase4553 "
    "  public constructor() end constructor; "
    "end CmLocal4553; "
    "public class CmMain4553 "
    "  public static method Main(): int "
    "    using var item := new CmLocal4553(); "
    "    using var holder := new CmHolder4553<CmLocal4553>(); "
    "    return holder.Describe(item).Length; "
    "  end Main; "
    "end CmMain4553; ";

// ── Row 2 sources — the constraint member is VIRTUAL and the consumer's type
// OVERRIDES it. Compiling the body correctly is not enough: the monomorphized
// call must dispatch through the instantiation's vtable, so the answer
// distinguishes "called the constraint's declaration" (3 + 1 = 4) from
// "dispatched to the consumer's override" (40 + 1 = 41).
const char* kVirtLib4553 =
    "module VtLib4553; "
    "public class VtBase4553 "
    "  public constructor() end constructor; "
    "  public virtual method Tag(): int => 3; "
    "end VtBase4553; "
    "public class VtHolder4553<T> where T: VtBase4553 "
    "  public constructor() end constructor; "
    "  public method Describe(item: T): int => item.Tag() + 1; "
    "end VtHolder4553; ";

const char* kVirtApp4553 =
    "module VtApp4553; "
    "import VtLib4553; "
    "public class VtLocal4553: VtBase4553 "
    "  public constructor() end constructor; "
    "  public override method Tag(): int => 40; "
    "end VtLocal4553; "
    "public class VtMain4553 "
    "  public static method Main(): int "
    "    using var item := new VtLocal4553(); "
    "    using var holder := new VtHolder4553<VtLocal4553>(); "
    "    return holder.Describe(item); "
    "  end Main; "
    "end VtMain4553; ";

// ── Row 3 sources — CONTROL. A constrained generic whose body does NOT use the
// constraint reconstructed correctly BEFORE this fix (the clause was simply
// dropped and nothing missed it). Rendering the clause must not take that away:
// the risk of the fix is a constraint the synthetic source cannot resolve
// turning a working reconstruction into a failed one.
const char* kPlainLib4553 =
    "module PlLib4553; "
    "public class PlBase4553 "
    "  public constructor() end constructor; "
    "  public method Tag(): int => 3; "
    "end PlBase4553; "
    "public class PlHolder4553<T> where T: PlBase4553 "
    "  public v: int; "
    "  public constructor() end constructor; "
    "  public method Describe(item: T): int => this.v + 7; "
    "end PlHolder4553; ";

const char* kPlainApp4553 =
    "module PlApp4553; "
    "import PlLib4553; "
    "public class PlLocal4553: PlBase4553 "
    "  public constructor() end constructor; "
    "end PlLocal4553; "
    "public class PlMain4553 "
    "  public static method Main(): int "
    "    using var item := new PlLocal4553(); "
    "    using var holder := new PlHolder4553<PlLocal4553>(); "
    "    holder.v := 60; "
    "    return holder.Describe(item); "
    "  end Main; "
    "end PlMain4553; ";

// ── Row 4 sources — GATE CANARY (the T5 shape of xmod_generic_method_783,
// re-stated here so the scope of THIS fix is pinned beside it). `Helper` is a
// public sibling that the body reaches but no constraint NAMES, so it is not
// registered, the reconstruction still fails, and the §14.1/#783 gate must
// still reject rather than emit a wrong value.
const char* kCanaryLib4553 =
    "module GcLib4553; "
    "public class GcHelper4553 "
    "  public static method Compute(): int return 99; end Compute; "
    "end GcHelper4553; "
    "public class GcBox4553<T> "
    "  public v: T; "
    "  public constructor() end constructor; "
    "  public method GetDirty(): T "
    "    var x := GcHelper4553.Compute(); "
    "    return this.v; "
    "  end GetDirty; "
    "end GcBox4553; ";

const char* kCanaryApp4553 =
    "module GcApp4553; "
    "import GcLib4553; "
    "public class GcMain4553 "
    "  public static method Main(): int "
    "    using var b := new GcBox4553<string>(); "
    "    b.v := \"hello\"; "
    "    return b.GetDirty().Length; "
    "  end Main; "
    "end GcMain4553; ";

}  // namespace

// ===========================================================================
// Row 1 — THE BUG (§2.8.2 / §14.2, v0.306). The rule corpus's `s2_8_2_0005`
// shape: a public generic constrained `where T: CmBase4553`, instantiated at a
// CONSUMER-declared `CmLocal4553`, whose body calls the member the constraint
// provides.
//
// PRE-FIX the importing compile FAILS — "Call parameter type does not match
// function signature!" from LLVM's verifier, because the call was lowered
// against the defining module's i32-placeholder template — so this helper
// returns the -3 sentinel. POST-FIX it returns 4, `"base".Length`.
//
// The single-module twin is measured in the same row because §2.8.2's rule is
// an EQUIVALENCE ("an external instantiation behaves identically to a
// same-module instantiation at the same arguments"), and it answered 4 on both
// compilers — which is exactly why the gap was invisible from one module.
// ===========================================================================
BEZEL_TEST(xmod_generic_constraint_member_4553,
           constraint_member_call_crosses_import_boundary_spec_2_8_2) {
    const int sameModule = bezel::test::runAot(kSingleModuleTwin4553).exitCode;
    BEZEL_EXPECT_EQ(4, sameModule);

    const int crossModule =
        buildImportAndRun4553(kLib4553, kApp4553, "wt-4553-cm");
    // 4 == "base".Length. -3 == the pre-#4553 importer, which could not build
    // the executable at all (IR verification failed).
    BEZEL_EXPECT_EQ(4, crossModule);
    BEZEL_EXPECT_EQ(sameModule, crossModule);
}

// ===========================================================================
// Row 2 — the constraint member is VIRTUAL and the consumer OVERRIDES it
// (§2.8.2 / §14.2 / §11.4, v0.306). Not a duplicate of row 1: row 1 proves the
// call is emitted at all, this row proves it is emitted as a per-instantiation
// VIRTUAL dispatch rather than a static call to the constraint's own
// declaration. 41 == the override ran (40 + 1); 4 would mean the base
// declaration ran; -3 is the pre-fix verifier failure.
// ===========================================================================
BEZEL_TEST(xmod_generic_constraint_member_4553,
           constraint_member_dispatches_to_consumer_override_spec_2_8_2) {
    const int rc =
        buildImportAndRun4553(kVirtLib4553, kVirtApp4553, "wt-4553-vt");
    BEZEL_EXPECT_EQ(41, rc);
}

// ===========================================================================
// Row 3 — CONTROL (§14.1, v0.306). A constrained generic whose body does NOT
// touch the constraint reconstructed correctly before this fix. Rendering the
// `where` clause must not regress it: if a rendered constraint failed to
// resolve in the synthetic source, the §14.1/#783 soundness gate would refuse
// the body and this previously-working program would start failing.
// 67 == 60 + 7 on both compilers.
// ===========================================================================
BEZEL_TEST(xmod_generic_constraint_member_4553,
           constrained_generic_with_unrelated_body_unaffected_spec_14_1) {
    const int rc =
        buildImportAndRun4553(kPlainLib4553, kPlainApp4553, "wt-4553-pl");
    BEZEL_EXPECT_EQ(67, rc);
}

// ===========================================================================
// Row 4 — GATE CANARY (§14.1 / #783, v0.306). The fix registers ONLY the types
// a constraint names. A body that reaches an arbitrary public sibling is still
// unreconstructable, so the §14.1/#783 gate must still REJECT the reference
// instantiation — never compile it to a wrong value. This row fails if a later
// change widens the registry to "every public sibling".
// ===========================================================================
BEZEL_TEST(xmod_generic_constraint_member_4553,
           non_constraint_sibling_reference_stays_gated_spec_14_1) {
    BEZEL_EXPECT_TRUE(
        buildImportFails4553(kCanaryLib4553, kCanaryApp4553, "wt-4553-gc"));
}

}  // namespace bezel::module
