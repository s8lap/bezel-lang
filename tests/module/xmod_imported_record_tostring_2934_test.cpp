// Regression tests for #2934's cross-module half (language spec §8.5 / §9.1 /
// §9.2 / §18.5, v0.252; §2.8.3 rule 4 "one runtime type identity").
//
// THE DEFECT, and why the single-module suite could not see it. §8.5 keeps a
// record HEAD unqualified — "positional-record and auto-equality formatting
// keeps the **unqualified** head (`"Point(X: 10, Y: 20)"`, §9.1)" — while the
// class / enum / non-auto-equality-body-record defaults carry the module path.
// Deciding which of those a record takes means asking "is this positional?",
// and codegen asked it of `TypeSymbol::decl` alone:
//
//     bool positional = false;
//     if (auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl))
//         positional = td->hasPositionalFields;
//
// `ModuleLoader` sets `TypeSymbol::hasPositionalFields` for an imported record
// and NEVER assigns `TypeSymbol::decl` — SymbolCollection.cpp says so at its own
// copy of the predicate ("Imported positional records (no ast::TypeDecl — only
// hasPositionalFields flag)"), and the sema copies read both sources. So for an
// IMPORTED positional record the codegen answer was `false`: the record was
// classified as an ineligible BODY record and took the name-only arm.
//
// MEASURED on f1175c6, `module Dep; public record Pt(X: int, Y: int);` imported
// by `module App;` — four spellings of ONE value, three of them disagreeing:
//
//     p.ToString()                  ->  "Dep.Pt"            <- a QUALIFIED
//                                                              record head,
//                                                              which §8.5
//                                                              forbids outright
//     var o: Object := p; o.ToString()
//                                   ->  "Pt(X: 1, Y: 2)"    <- correct
//     $"{p}"                        ->  "Pt"                <- unqualified head,
//                                                              but the field
//                                                              listing dropped
//     Console.WriteLine(p)          ->  "Pt"                <- #3118, separate
//
// Before #2934 all four agreed on "Pt" — wrong in the same way, so no test
// noticed. #2934 made the disagreement visible by moving one of them.
//
// The fix is one shared predicate (`Emitter::recordIsPositional`) that reads
// BOTH sources, called from every §8.5 site. These tests are the cross-module
// coverage that was missing: `tests/module/` exercised the BOXED spelling of an
// imported record (xmod_imported_record_box_2796) and never the direct unboxed
// `.ToString()`, which is why a 6,752-test sweep came back clean on a live
// defect.
//
// Each test asserts the rendered TEXT of all four spellings in one program, so
// a future divergence between them fails here rather than in whichever one a
// later test happens to pick.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"   // codegen::CodegenOptions
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // #3156: the ONE sanitizer-honouring compileFile wrapper
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {
namespace fs = std::filesystem;

// Per-process unique working dir under the shared scratch base. The previous
// fs::temp_directory_path() (== /tmp tmpfs) + non-atomic counter pattern was
// race-prone: parallel shards overwrote each other's _lib.bzl / _lib.o /
// _main.bzl mid-compileObject and garbled the in-flight child read. The atomic
// unique_stem + /var/tmp/bezel-scratch parent fixes that class of flake
// (#3002-class COVERAGE SHORT).
const std::filesystem::path& wtDir2934() {
    return wtScratchDir("wt-2934");
}

struct TempFile2934 {
    std::string path;
    explicit TempFile2934(std::string p) : path(std::move(p)) {}
    ~TempFile2934() { std::remove(path.c_str()); }
};

void writeSource2934(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib -> object, compile main importing it -> executable, run it and
// CAPTURE STDOUT. The rendering is the observable here, so the exit code alone
// (the shape xmod_imported_class_vtable_2792's helper returns) is not enough.
// Returns the captured stdout, or the sentinel "<did-not-run>" when either
// compile, the link, or the run fails — mirroring runAotOut's idiom so a
// toolchain problem cannot be read as a wrong rendering.
//
// ⚠ THE GENERATED CODE IS ASan/LSan-INSTRUMENTED (`opts.sanitizeAddress`), and
// that is load-bearing, not belt-and-braces. This helper hand-rolls
// `Driver::compileFile` rather than going through `runAot`, and
// `BEZEL_AOT_FORCE_SANITIZE` — the knob the gate's `--full` oracle sets — is
// honoured ONLY inside `runAot` (tests/support/aot_support.hpp). So a program
// compiled here is invisible to BOTH gate oracles unless this helper asks for
// the instrumentation itself. It does, following the established pattern next
// door in xmod_object_tostring_producer_residual_2867_test.cpp.
//
// LSan forces the process exit code to 1 on any leak, and this helper maps a
// non-zero exit to "<did-not-run>" — so **every expectation below is also a
// leak assertion**, and a leaked render fails as a rendering mismatch with the
// sanitizer report on stderr. That matters here specifically: #2934 changed an
// imported positional record's `ToString()` from a pooled literal sentinel to a
// freshly minted rc=1 string, and the +1 recognizer
// (`Emitter::isPlusOneStringProducer`) had to learn the same predicate in the
// same commit or the bind would retain it and leak the producer's reference.
// MEASURED with the recognizer left behind: 962 bytes / 40 allocations over 20
// iterations. `imported_positional_record_render_is_leak_free` is the amplified
// form; the other tests carry the same net at one iteration each.
std::string runXmodOut2934(const std::string& libSrc,
                           const std::string& mainSrc) {
    auto dir = wtDir2934();
    std::string stem = unique_stem("bezel_xmod2934");
    TempFile2934 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile2934 lObj((dir / (stem + "_lib.o")).string());
    TempFile2934 mSrc((dir / (stem + "_main.bzl")).string());
    TempFile2934 mExe((dir / (stem + "_main")).string());
    TempFile2934 mOut((dir / (stem + "_out.txt")).string());

    writeSource2934(lSrc.path, libSrc);
    writeSource2934(mSrc.path, mainSrc);

    bezel::Driver driver;
    if (!driver.compileObject(lSrc.path, lObj.path).success)
        return "<did-not-run>";

    bezel::Dependency dep;
    dep.metadataPath = lObj.path;
    dep.objectPath   = lObj.path;
    std::vector<bezel::Dependency> deps{dep};
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;   // see the note above: this IS the leak gate
    if (!bezel::test::compileFileForTest(driver, mSrc.path, mExe.path, deps, opts).success)
        return "<did-not-run>";

    std::string cmd = mExe.path + " > " + mOut.path + " 2>/dev/null";
    int status = std::system(cmd.c_str());
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return "<did-not-run>";

    std::ifstream in(mOut.path);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// The dependency: one POSITIONAL record (§9.1), one AUTO-EQUALITY body record
// (§9.2), and one body record that is NOT auto-hashable (an explicit field is
// §9.2's own disqualifier) — the three record rows of §8.5's three-way split,
// exported so the importer sees them only through module metadata.
const char* kDep2934 =
    "module Dep2934; "
    "public record Pt(X: int, Y: int); "
    "public record Tag "
    "  public property N: int; "
    "  public constructor(n: int) N := n; end constructor; "
    "end Tag; "
    "public record Opaque "
    "  public property A: int; "
    "  private _cache: int := 0; "
    "  public constructor(a: int) A := a; end constructor; "
    "end Opaque; "
    "public class Widget public Id: int := 0; end Widget; "
    "public enum Colour Red, Green; end Colour;";

}  // namespace

// ---------------------------------------------------------------------------
// §9.1 — the imported POSITIONAL record. Head UNQUALIFIED, fields listed, and
// all three ToString spellings must agree.
// ---------------------------------------------------------------------------

BEZEL_TEST(xmod_imported_record_tostring_2934,
           imported_positional_record_head_stays_unqualified) {
    BEZEL_EXPECT_EQ(runXmodOut2934(kDep2934,
        "module App2934A; import Dep2934; "
        "public class C public static method Main(): int "
        "  var p := new Pt(1, 2); "
        "  Console.WriteLine(p.ToString()); "
        "  return 0; "
        "end Main; end C;"),
        "Pt(X: 1, Y: 2)\n");
}

// The disagreement the defect produced, pinned as an equality between the three
// spellings rather than three separate expectations — this is the property
// §8.5 guarantees ("`ToString` is invoked implicitly inside string
// interpolation via virtual dispatch"), and the one that actually broke.
BEZEL_TEST(xmod_imported_record_tostring_2934,
           imported_positional_record_all_spellings_agree) {
    BEZEL_EXPECT_EQ(runXmodOut2934(kDep2934,
        "module App2934B; import Dep2934; "
        "public class C public static method Main(): int "
        "  var p := new Pt(1, 2); "
        "  var call := p.ToString(); "
        "  var interp := $\"{p}\"; "
        "  var o: Object := p; "
        "  var boxed := o.ToString(); "
        "  Console.WriteLine(call); "
        "  Console.WriteLine((if interp = call then \"AGREE\" "
        "                     else \"DISAGREE:\" + interp)); "
        "  Console.WriteLine((if boxed = call then \"AGREE\" "
        "                     else \"DISAGREE:\" + boxed)); "
        "  return 0; "
        "end Main; end C;"),
        "Pt(X: 1, Y: 2)\nAGREE\nAGREE\n");
}

// The amplified form of the same shape, named so the failure reads as what it
// is. Twenty iterations of `var s := p.ToString();` on an imported positional
// record: the rendering is a fresh rc=1 string, and the local bind must TRANSFER
// that reference rather than retain it. LSan's exit code is the assertion (this
// helper maps any non-zero exit to "<did-not-run>"), so the printed marker only
// has to prove the program ran to completion.
BEZEL_TEST(xmod_imported_record_tostring_2934,
           imported_positional_record_render_is_leak_free) {
    BEZEL_EXPECT_EQ(runXmodOut2934(kDep2934,
        "module App2934F; import Dep2934; "
        "public class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    var p := new Pt(i, i); "
        "    var s: string := p.ToString(); "
        "    var t: string := $\"{p}\"; "
        "    if s.Length < 1 then return 7; end; "
        "    if t.Length < 1 then return 8; end; "
        "  end while; "
        "  Console.WriteLine(\"CLEAN\"); "
        "  return 0; "
        "end Main; end C;"),
        "CLEAN\n");
}

// ---------------------------------------------------------------------------
// §9.2 — the imported AUTO-EQUALITY body record. Head unqualified, properties
// listed. (This row never depended on `ts->decl`: it goes through
// `isAutoEqualityBearingBodyRecord`, which already read the metadata. Kept as
// the control that localizes the defect to the POSITIONAL row.)
// ---------------------------------------------------------------------------

// ⚠ THE PROPERTY VALUE IS DELIBERATELY NOT ASSERTED, and the reason is a
// separate live defect: #3130 — an IMPORTED auto-equality body record's
// constructor does not store, so `new Tag(41)` yields `N = 0` and even the
// plain member read `t.N` answers 0. That is a STORAGE defect, measured before
// any formatting, and this file must not encode its wrong value as an
// expectation (which would silently invert into a failure the day #3130 is
// fixed). What §8.5 governs here — the UNQUALIFIED head, and a property LISTING
// rather than a bare name — is asserted in full; the number inside the listing
// is #3130's to fix.
BEZEL_TEST(xmod_imported_record_tostring_2934,
           imported_auto_equality_body_record_head_stays_unqualified) {
    std::string out = runXmodOut2934(kDep2934,
        "module App2934C; import Dep2934; "
        "public class C public static method Main(): int "
        "  var t := new Tag(41); "
        "  var call := t.ToString(); "
        "  var interp := $\"{t}\"; "
        "  Console.WriteLine((if call.StartsWith(\"Tag(N: \") "
        "                     then \"HEAD-OK\" else \"HEAD-BAD:\" + call)); "
        "  Console.WriteLine((if call.EndsWith(\")\") "
        "                     then \"SHAPE-OK\" else \"SHAPE-BAD:\" + call)); "
        "  Console.WriteLine((if interp = call then \"AGREE\" "
        "                     else \"DISAGREE:\" + interp)); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(out, "HEAD-OK\nSHAPE-OK\nAGREE\n");
}

// ---------------------------------------------------------------------------
// §8.5's other two rows, cross-module: an imported NON-auto-equality body
// record and an imported class/enum DO carry the module path — and it is the
// EXPORTER's module, not the importer's (#1488's `TypeSymbol::moduleName`,
// which ModuleLoader restores from the dependency's metadata). These are what
// make the positional row's unqualified head a real distinction rather than an
// accident of the cross-module path losing the module name.
// ---------------------------------------------------------------------------

BEZEL_TEST(xmod_imported_record_tostring_2934,
           imported_ineligible_body_record_is_module_qualified) {
    BEZEL_EXPECT_EQ(runXmodOut2934(kDep2934,
        "module App2934D; import Dep2934; "
        "public class C public static method Main(): int "
        "  var m := new Opaque(3); "
        "  Console.WriteLine(m.ToString()); "
        "  Console.WriteLine($\"{m}\"); "
        "  return 0; "
        "end Main; end C;"),
        "Dep2934.Opaque\nDep2934.Opaque\n");
}

BEZEL_TEST(xmod_imported_record_tostring_2934,
           imported_class_and_enum_carry_the_exporters_module) {
    BEZEL_EXPECT_EQ(runXmodOut2934(kDep2934,
        "module App2934E; import Dep2934; "
        "public class C public static method Main(): int "
        "  using var w := new Widget(); "
        "  var c := Colour.Green; "
        "  Console.WriteLine(w.ToString()); "
        "  Console.WriteLine(c.ToString()); "
        "  return 0; "
        "end Main; end C;"),
        "Dep2934.Widget\nDep2934.Colour\n");
}

// ---------------------------------------------------------------------------
// §2.8.3 rule 4: the SAME declarations compiled into ONE module must render
// identically apart from the module name. The single-module control is what
// proves the cross-module path is the variable — without it, a shared defect in
// the renderer would satisfy every expectation above.
// ---------------------------------------------------------------------------

BEZEL_TEST(xmod_imported_record_tostring_2934,
           single_module_control_renders_the_same_shapes) {
    BEZEL_EXPECT_EQ(runXmodOut2934(
        "module Unused2934; public class Z public N: int := 0; end Z;",
        "module Solo2934; "
        "public record Pt(X: int, Y: int); "
        "public record Opaque "
        "  public property A: int; "
        "  private _cache: int := 0; "
        "  public constructor(a: int) A := a; end constructor; "
        "end Opaque; "
        "public class C public static method Main(): int "
        "  var p := new Pt(1, 2); "
        "  var m := new Opaque(3); "
        "  Console.WriteLine(p.ToString()); "
        "  Console.WriteLine(m.ToString()); "
        "  return 0; "
        "end Main; end C;"),
        "Pt(X: 1, Y: 2)\nSolo2934.Opaque\n");
}

}  // namespace bezel::module
