// fix #2379 (§11.2 / §13 / §2.8.2 rule 2 / §2.8.3 rule 4, v0.234): cross-module
// dispatch through a GENERIC INTERFACE-typed reference (CG35-itable) silently
// emitted NO CALL AT ALL — a call whose destination local was never stored to,
// so the read observed whatever garbage happened to be on the stack (repro:
// `v=-943274556`) — for EVERY instantiation of an imported generic interface's
// method, value-type or reference-type alike. This is NOT the #783/§14.1
// "placeholder-ABI body" defect (that concern is specific to calling a method
// DIRECTLY on an imported generic CLASS, whose body is reconstructed importer-
// side against an i32 placeholder, #783/#1937): an interface member has NO
// BODY by construction (§11.2 — "Interface members are ... have no body"), so
// the actual callee reached through interface dispatch is always the fully
// concrete, locally-compiled body of whichever class implements the interface.
// There is no placeholder ABI involved anywhere in this path.
//
// ROOT CAUSE (found via BEZEL_DUMP_IR=1 on the issue's exact repro): CG35-
// itable dispatch keys BOTH the object's itmap entries (declareVtables Pass D,
// LlvmBackend_Setup.cpp) and the `bz_itable_lookup` call site
// (LlvmBackend_Calls2.cpp) on `descArray_[iface]` — the interface DECLARATION's
// own type-erased identity global (`td$<Module>.<Interface>`), independent of
// any type argument (the itable holds the implementing class's already-
// concrete method pointer; T never reaches this key). Before this fix, BOTH
// the export-side loop (which gives a public type's descriptor EXTERNAL
// linkage) and the import-side loop (which declares an external reference to
// it) explicitly excluded every generic class/interface
// (`!ts->genericParams.empty()`) — a carve-out that was correct for a generic
// CLASS (whose cross-module identity is per-instantiation, minted locally by
// `instanceDescriptorFor`) but wrong for a generic INTERFACE, whose itable key
// is the shared, declaration-level identity and needs no per-instantiation
// counterpart at all. With no `descArray_[iface]` entry for an imported
// generic interface: (a) `declareVtables`'s itmap-builder's
// `if (dit2 != descArray_.end())` guard silently skipped pushing the
// `[td$I, itab$C$I]` pair into the implementing class's own itmap, so the
// itable it *did* build (with the correct function pointer) was never
// reachable via lookup; and (b) the call-site's own identical guard
// (`if (descIt == descArray_.end()) { return nullptr; }`) silently dropped
// the whole call — no callee invoked, no store to the destination local.
//
// THE FIX: `LlvmBackend_Setup.cpp` — both loops now also export/declare a
// public GENERIC INTERFACE's shared descriptor (kept excluded for generic
// CLASSES, whose situation is unrelated and unchanged). `ExprTypecheck_Calls.cpp`
// — the pre-existing §14.1/#783 diagnostic (which incidentally rejected the
// REFERENCE-type-argument half of this exact bug, via `refLike`, while its
// `int`/value-type half slipped through with no diagnostic at all — the
// "silently wrong values, no crash" class this whole run is hunting) is
// narrowed to `ms->parent->kind == ast::TypeDeclKind::Class`: its placeholder-
// ABI rationale never applied to an interface parent (see above), and with the
// itable fix landed, interface dispatch is correct for EVERY instantiation, so
// keeping the reject there would newly reject now-correct programs — §2.8.2
// (v0.234) is explicit that an implementation unable to carry a shape "must
// not alter the instantiation's semantics" once it genuinely carries the
// shape.
//
// VERIFIED BEFORE THIS TEST EXISTED (probe, then mechanically: revert the two
// src/ hunks via a scratch-copied patch, rebuild, re-probe, reapply):
//   T1 (int, return position):     before v=-943274556, after v=42
//   T2 (int, parameter position):  before v=-943274556, after v=42
//   T3 (string, return position):  before REJECTED by §14.1/#783 (refLike),
//                                   after v="hi" (Length 2), correctly compiled
//   same-module control:           v=42 on both sides (never regressed)

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2379 {
    std::string path;
    explicit TempFile2379(std::string p) : path(std::move(p)) {}
    ~TempFile2379() { std::remove(path.c_str()); }
};

void writeSource2379(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2379(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B (importing A's already-built object) -> exe, run, return the exit
// code (-1 on compile/link/run failure, -2 if the dependency object itself
// could not be produced — toolchain unavailable, callers SKIP on this).
int compileImportAndRun2379(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2379_" + std::to_string(std::rand());
    TempFile2379 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2379 aObj((dir / (stem + "_a.o")).string());
    writeSource2379(aSrc.path, libSrc);
    if (!compileToObj2379(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2379 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2379 bExe((dir / stem).string());
    writeSource2379(bSrc.path, appSrc);
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

// Single-module (no import) compile+run, for the same-module control.
int compileAndRun2379(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2379_same_" + std::to_string(std::rand());
    TempFile2379 srcFile((dir / (stem + ".bzl")).string());
    TempFile2379 exe((dir / stem).string());
    writeSource2379(srcFile.path, src);
    bezel::Driver driver;
    if (!driver.compileFile(srcFile.path, exe.path, {}).success) return -1;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — PRIMARY REPRO: a correct implementation of a cross-module GENERIC
// interface member, VALUE-type (int) instantiation, RETURN position. Issue
// #2379's exact repro. Before the fix: v=-943274556 (garbage — no callee was
// ever invoked). After: v=42.
// ===========================================================================
BEZEL_TEST(xmod_generic_iface_dispatch_2379,
           value_type_int_return_position_dispatches_correctly_11_2) {
    int r = compileImportAndRun2379(
        "module IBoxLib2379T1; "
        "public interface IBox<T> method Get(): T; end IBox;",
        "module App2379T1; import IBoxLib2379T1; "
        "public class RightBox: IBox<int> "
        "  public method Get(): int => 42; "
        "end RightBox; "
        "public class C public static method Main(): int "
        "  using var b: IBox<int> := new RightBox(); "
        "  return b.Get(); "
        "end Main; end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T2 — value-type (int) instantiation, PARAMETER position (issue #2379 notes
// this reproduces identically — "not return-position-specific"). Before: the
// same garbage value; after: 42 (41 + 1).
// ===========================================================================
BEZEL_TEST(xmod_generic_iface_dispatch_2379,
           value_type_int_parameter_position_dispatches_correctly_11_2) {
    int r = compileImportAndRun2379(
        "module IAcceptLib2379T2; "
        "public interface IAccept<T> method Accept(x: T): int; end IAccept;",
        "module App2379T2; import IAcceptLib2379T2; "
        "public class IntAccepter: IAccept<int> "
        "  public method Accept(x: int): int => x + 1; "
        "end IntAccepter; "
        "public class C public static method Main(): int "
        "  using var a: IAccept<int> := new IntAccepter(); "
        "  return a.Accept(41); "
        "end Main; end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T3 — REFERENCE-type (string) instantiation, RETURN position. Before this
// fix this shape was REJECTED at compile time by the §14.1/#783 diagnostic
// (a clean-but-overbroad guard: it fired on every interface-dispatch call
// through an imported generic interface — its stated placeholder-ABI
// rationale never actually applied to an interface parent). After the fix it
// compiles AND dispatches correctly: "hi".Length == 2.
// ===========================================================================
BEZEL_TEST(xmod_generic_iface_dispatch_2379,
           reference_type_string_return_position_dispatches_correctly_11_2) {
    int r = compileImportAndRun2379(
        "module IBoxLib2379T3; "
        "public interface IBox<T> method Get(): T; end IBox;",
        "module App2379T3; import IBoxLib2379T3; "
        "public class RightBox: IBox<string> "
        "  public method Get(): string => \"hi\"; "
        "end RightBox; "
        "public class C public static method Main(): int "
        "  using var b: IBox<string> := new RightBox(); "
        "  var v: string := b.Get(); "
        "  return v.Length; "
        "end Main; end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 2);
}

// ===========================================================================
// T4 — ANOTHER value-type ABI shape (double, f64 — not the i32 that `int`
// happens to share with the old placeholder mechanism), to confirm this is a
// genuinely T-independent fix (the itable key never encodes T at all) rather
// than an accidental int-only match.
// ===========================================================================
BEZEL_TEST(xmod_generic_iface_dispatch_2379,
           value_type_double_return_position_dispatches_correctly_11_2) {
    int r = compileImportAndRun2379(
        "module IBoxLib2379T4; "
        "public interface IBox<T> method Get(): T; end IBox;",
        "module App2379T4; import IBoxLib2379T4; "
        "public class RightBox: IBox<double> "
        "  public method Get(): double => 4.0; "
        "end RightBox; "
        "public class C public static method Main(): int "
        "  using var b: IBox<double> := new RightBox(); "
        "  var v: double := b.Get(); "
        "  return (if v > 3.5 then 1 else 0); "
        "end Main; end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 1);
}

// ===========================================================================
// T5 — SAME-MODULE CONTROL: the equivalent single-module program (no import
// boundary at all) must keep returning the correct value — guards that the
// descriptor-export widening in LlvmBackend_Setup.cpp does not perturb
// single-module (already-internal) descriptor linkage or itmap construction.
// ===========================================================================
BEZEL_TEST(xmod_generic_iface_dispatch_2379,
           same_module_control_unaffected_11_2) {
    int r = compileAndRun2379(
        "module Same2379T5; "
        "public interface IBox<T> method Get(): T; end IBox; "
        "public class RightBox: IBox<int> "
        "  public method Get(): int => 42; "
        "end RightBox; "
        "public class C public static method Main(): int "
        "  using var b: IBox<int> := new RightBox(); "
        "  return b.Get(); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r, 42);
}

}  // namespace bezel::module
