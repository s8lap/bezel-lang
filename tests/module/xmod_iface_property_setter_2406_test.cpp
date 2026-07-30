// fix #2406 (§23 / §11.2 / §2.8.2 / §2.8.3 rule 4, v0.235): `Emitter::
// propertyHasSetter` (`LlvmBackend_impl.hpp`) read `ps->decl` as an
// `ast::PropertyDecl*` with no fallback, which is unconditionally null for
// any IMPORTED PropertySymbol (ModuleLoader.cpp's `MetaMemberKind::Property`
// case: "the getter BODY is not reconstructed here... there is no
// ast::PropertyDecl in the importer"). Called on an IMPORTED INTERFACE's own
// read-write PropertySymbol — to decide whether `declareVtables`'s Pass 0
// (and its #2401 import-mirror) should reserve an itable setter slot at all
// for that interface member — `propertyHasSetter` unconditionally returned
// `false`, so NO setter itable slot was ever reserved for a read-write
// property declared on an IMPORTED interface: `b.Name := v` through such a
// reference silently no-op'd, even when the implementing class and its
// setter accessor were entirely LOCAL (no cross-module accessor EXPORT
// needed at all — the narrower, orthogonal gap #2400 separately fixed).
//
// Filed as the fifteenth confirmed instance of #2374's audit pattern (an
// `astNode`/`decl`-only gate silently mishandling an imported symbol).
//
// THE FIX has two parts, both required (confirmed necessary by probe — the
// #2387 lesson: verify the runtime OUTCOME, not that a fix compiles):
//   1. `propertyHasSetter` now falls back to
//      `ctx_.importedPropertyHasSetter(ps)` when `ps->decl` is null. That
//      side table is `MetaMember::hasSetter`, a metadata bit #2400 already
//      added (threaded builder -> loader -> SemaContext) for its own,
//      narrower "direct cross-module set" shape — reused here rather than
//      invented fresh. A naive `ps->modifiers`-only heuristic ("no
//      `readonly` means a setter exists") was considered and REJECTED:
//      confirmed by probe to be unsound. Bezel_Language_Spec.md §22.3/§23.1
//      (v0.235): "on a block form declaring a setter [readonly] is
//      contradictory and likewise an error" — and equally an error on a
//      get-only block form, since the shape already expresses
//      read-onlyness. So `readonly` NEVER appears on a block-form
//      declaration's modifiers regardless of whether it declares a `set`
//      arm — a get-only block-form interface property and a read-write
//      auto-form interface property are INDISTINGUISHABLE by modifiers
//      alone. Probed before choosing the real metadata bit: the naive
//      fallback would have reserved a PHANTOM setter itable slot for a
//      get-only block-form imported interface property; with no real setter
//      anywhere to fill it, `declareVtables` Pass D installs
//      ConstantPointerNull, and a write reaching that slot at runtime calls
//      a null function pointer — the exact SIGSEGV shape #2387 fixed for
//      getters. T3/T4 below are the regression guard pinning that this
//      crash was NOT introduced.
//   2. `Emitter::lowerVirtualPropertySet` (`LlvmBackend_Exprs.cpp`) — the
//      actual SET *dispatch*, as opposed to the itable *slot reservation*
//      question `propertyHasSetter` answers — had the identical `!pd ->
//      return false` bailout on the value type it needs to build the call.
//      Fixing only `propertyHasSetter` reserves the slot but this function
//      still bails before ever reaching it, so the write remains a silent
//      no-op even after part 1 (confirmed by probe). Fixed to fall back to
//      `ctx_.importedMemberType(ps)`, mirroring `lowerVirtualPropertyGet`'s
//      pre-existing #503 fallback immediately above it in the same file.
//
// MECHANICAL PROOF (this file's T1, confirmed by probe/scratch-patch cycle,
// NOT `git stash`): pre-fix, `b.Name := 42; return b.Name + 100;` returns
// `101` (the set silently no-op'd, `_n` stayed at its initializer `1`);
// post-fix, `142` (`42 + 100`).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2406 {
    std::string path;
    explicit TempFile2406(std::string p) : path(std::move(p)) {}
    ~TempFile2406() { std::remove(path.c_str()); }
};

void writeSource2406(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2406(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B (importing A's compiled object) -> exe, run it, and return its
// exit code (-1 on compile/link/run failure, -2 if the dependency itself
// could not be produced). Mirrors compileImportAndRun2401/2380/2387.
int compileImportAndRun2406(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2406_" + std::to_string(std::rand());
    TempFile2406 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2406 aObj((dir / (stem + "_a.o")).string());
    writeSource2406(aSrc.path, libSrc);
    if (!compileToObj2406(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2406 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2406 bExe((dir / stem).string());
    writeSource2406(bSrc.path, appSrc);
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
// T1 — the exact #2406 repro (matrix shape (c): imported interface's own
// AUTO-form read-write property, implemented entirely LOCALLY). Pre-fix:
// `101` (the set silently no-op'd). Post-fix: `142` (`42 + 100`), proving
// the write reached `_n`, not just that `b.Name` still reads its original
// value.
// ===========================================================================
BEZEL_TEST(xmod_iface_property_setter_2406,
           auto_form_setter_through_imported_interface_dispatches_value) {
    int r = compileImportAndRun2406(
        "module SetPropLibC2406; "
        "public interface IHasNameSetC2406 "
        "  property Name: int; "
        "end IHasNameSetC2406;",
        "module AppSetC2406; import SetPropLibC2406; "
        "public class DerivedGoodSetC2406: IHasNameSetC2406 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end DerivedGoodSetC2406; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: IHasNameSetC2406 := new DerivedGoodSetC2406(); "
        "    b.Name := 42; "
        "    return b.Name + 100; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 142);
}

// ===========================================================================
// T2 — BLOCK-form (explicit `get; set; end;`) read-write imported interface
// property, implemented locally. Isolates the block-form shape from T1's
// Auto-form shape: `propertyHasSetter`'s import fallback must answer `true`
// here too, even though the interface's own modifiers carry no `readonly`
// tag either way (block form never spells `readonly` — see file header).
// ===========================================================================
BEZEL_TEST(xmod_iface_property_setter_2406,
           block_form_setter_through_imported_interface_dispatches_value) {
    int r = compileImportAndRun2406(
        "module BlockFormLib2406; "
        "public interface IHasNameBlockRW2406 "
        "  property Name: int "
        "    get; "
        "    set; "
        "  end; "
        "end IHasNameBlockRW2406;",
        "module AppBlockRW2406; import BlockFormLib2406; "
        "public class ImplRW2406: IHasNameBlockRW2406 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end ImplRW2406; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: IHasNameBlockRW2406 := new ImplRW2406(); "
        "    b.Name := 42; "
        "    return b.Name + 100; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 142);
}

// ===========================================================================
// T3 — REGRESSION GUARD (the crash risk this fix's design deliberately
// avoided, per the file header): a BLOCK-form GET-ONLY imported interface
// property (no `set` arm — the shape indistinguishable from a read-write
// Auto-form property by `modifiers` alone). `b.Name := 42` must NOT crash.
// Per a pre-existing, separately-filed sema gap (#2412: assigning to a
// getter-only IMPORTED property is silently accepted, not rejected per
// §23.3), the write compiles and currently still silently no-ops — this
// test pins that it stays a SAFE no-op (`b.Name` reads back its original `5`,
// unaffected) rather than dispatching through a null itable slot, not that
// the accept itself is correct (that is #2412's territory, not this fix's).
// ===========================================================================
BEZEL_TEST(xmod_iface_property_setter_2406,
           block_form_getter_only_imported_interface_write_does_not_crash) {
    int r = compileImportAndRun2406(
        "module BlockFormGetOnlyLib2406; "
        "public interface IHasNameBlockGetOnly2406 "
        "  property Name: int "
        "    get; "
        "  end; "
        "end IHasNameBlockGetOnly2406;",
        "module AppBlockGetOnly2406; import BlockFormGetOnlyLib2406; "
        "public class ImplGetOnly2406: IHasNameBlockGetOnly2406 "
        "  public property Name: int => 5; "
        "end ImplGetOnly2406; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: IHasNameBlockGetOnly2406 := new ImplGetOnly2406(); "
        "    b.Name := 42; "
        "    return b.Name; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 5);
}

// ===========================================================================
// T4 — REGRESSION GUARD: an explicitly `readonly` AUTO-form imported
// interface property. `b.Name := 42` must NOT crash (same #2412-scoped
// silent-no-op caveat as T3) and the itable getter slot must still read the
// real value — guards that the `readonly` branch of `propertyHasSetter`'s
// fallback (checked via the reconstructed `hasSetter` bit, not a `readonly`
// modifier scan) still correctly answers `false` for an imported property.
// ===========================================================================
BEZEL_TEST(xmod_iface_property_setter_2406,
           readonly_auto_form_imported_interface_write_does_not_crash) {
    int r = compileImportAndRun2406(
        "module ReadonlyLib2406; "
        "public interface IHasNameReadonly2406 "
        "  readonly property Name: int; "
        "end IHasNameReadonly2406;",
        "module AppReadonly2406; import ReadonlyLib2406; "
        "public class ImplReadonly2406: IHasNameReadonly2406 "
        "  public property Name: int => 7; "
        "end ImplReadonly2406; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: IHasNameReadonly2406 := new ImplReadonly2406(); "
        "    b.Name := 42; "
        "    return b.Name; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 7);
}

// ===========================================================================
// T5 — CONTROL: matrix shape (a) — a wholly LOCAL property satisfying a
// wholly LOCAL interface's setter obligation (no import anywhere). Guards
// against the fix regressing ordinary same-module interface-setter dispatch.
// ===========================================================================
BEZEL_TEST(xmod_iface_property_setter_2406,
           local_interface_local_property_setter_control_unaffected) {
    bezel::Driver driver;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2406_t5_" + std::to_string(std::rand());
    TempFile2406 src((dir / (stem + ".bzl")).string());
    TempFile2406 exe((dir / stem).string());
    writeSource2406(src.path,
                    "module LocalSetA2406; "
                    "public interface IHasNameSetLocalA2406 "
                    "  property Name: int; "
                    "end IHasNameSetLocalA2406; "
                    "public class DerivedGoodSetLocalA2406: "
                    "IHasNameSetLocalA2406 "
                    "  private _n: int := 1; "
                    "  public property Name: int "
                    "    get => _n; "
                    "    set => _n := value; "
                    "  end Name; "
                    "end DerivedGoodSetLocalA2406; "
                    "public class C "
                    "  public static method Main(): int "
                    "    using var b: IHasNameSetLocalA2406 := "
                    "new DerivedGoodSetLocalA2406(); "
                    "    b.Name := 42; "
                    "    return b.Name + 100; "
                    "  end Main; "
                    "end C;");
    BEZEL_EXPECT_TRUE(driver.compileFile(src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 142);
}

// ===========================================================================
// T6 — REGRESSION GUARD (coordinator-reported false-positive attribution,
// investigated 2026-07-30): the gate's `is_transitive_generic_2192.
// is_transitive2_reified_int_matches_2192_14_1` (tests/codegen/
// is_transitive_generic_instantiation_2192_test.cpp) was reported failing
// with this fix's two source files present, and passing with them reverted.
// Pinned here verbatim as a permanent regression guard regardless of root
// cause. The program is PURELY SINGLE-MODULE with NO imports and NO
// properties anywhere (`Wrap<W>`'s `tag` is a plain FIELD, not a property;
// `A`/`B`/`D` declare only methods) — three levels of generic nesting
// (`D<int> -> B<int> -> A<int>`) doing a reified `m is Wrap<T>` check.
//
// DIAGNOSIS PERFORMED (per the coordinator's "do not guess" instruction,
// summarized fully in the fix-lane report; not reproduced here in this
// comment's brevity):
//   - gdb breakpoint counts compiling this EXACT program: `Emitter::
//     propertyHasSetter` — 0 hits; `Emitter::lowerVirtualPropertySet` — 0
//     hits (`lowerVirtualPropertyGet`, UNCHANGED by this fix, hit 27 times —
//     irrelevant to this diff). Neither of this fix's two changed functions
//     is reachable for a program with no properties and no interfaces at
//     all, which rules out the coordinator's own itable/vtable-layout-shift
//     hypothesis by direct measurement, not argument.
//   - `BEZEL_DUMP_IR=1` on this exact program: BYTE-IDENTICAL LLVM IR with
//     this fix's two files present vs. reverted (0-line diff), reproduced
//     across 3 fresh compiles each way — the compiler's output for this
//     program is therefore provably unaffected by this fix.
//   - the isolated gtest (`BEZEL_TEST_FILTER=
//     is_transitive2_reified_int_matches_2192_14_1`) PASSED on every attempt
//     with this fix applied: 8 repeated single-test runs, 3 with the AOT
//     compile cache disabled, 5 against a ccache-disabled from-scratch
//     rebuild, 3 under `BEZEL_AOT_FORCE_SANITIZE=address`, and the whole
//     14-test `is_transitive_generic_2192` suite run together once — 22
//     process invocations, 0 failures.
// Root cause, if the report reflects a real (not shard-contention / build-
// staleness) failure, is therefore NOT in this fix's changed code paths.
// Kept here as a value-asserting guard regardless: if this specific shape
// ever regresses for ANY reason while this fix's files are present, this
// test catches it without relying on cross-file attribution.
// ===========================================================================
BEZEL_TEST(xmod_iface_property_setter_2406,
           regression_guard_2192_transitive_generic_is_check_unrelated) {
    auto r = bezel::test::runAot(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class A<T> public method probe(m: Object, w: Object): int "
        "  => (if m is Wrap<T> then 1 else 0) * 2 "
        "   + (if w is Wrap<T> then 1 else 0); end A; "
        "class B<U> public method run(m: Object, w: Object): int "
        "  using var a := new A<U>(); "
        "  return a.probe(m, w); end run; end B; "
        "class D<X> public method run2(m: Object, w: Object): int "
        "  using var b := new B<X>(); "
        "  return b.run(m, w); end run2; end D; "
        "class C public static method Main(): int "
        "  using var wi := new Wrap<int>(1); "
        "  using var ws := new Wrap<string>(2); "
        "  var mi: Object := wi; var wsx: Object := ws; "
        "  using var d := new D<int>(); "
        "  return d.run2(mi, wsx); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

}  // namespace bezel::module
