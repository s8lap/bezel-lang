// fix #5876 — §19.3 whitelist bullet 3 / §7.4 (v0.334). The "a `readonly`
// field outside its construction window" case of §19.3's provably-unwritten
// whitelist was answered off `FieldSymbol::decl`, and that member is null for
// EVERY IMPORTED field.
//
// ── WHAT THE DEFECT WAS ──────────────────────────────────────────────────
//
// `ExprTC::caseSourceProvablyUnwritten` (src/sema/ExprTypecheck.cpp:25495)
// is the ONE function that answers §19.3's whitelist for a member-access
// scrutinee. Its field arm read the fact like this:
//
//     if (fs->decl) {
//       if (auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl)) {
//         for (auto m : fd->modifiers)
//           if (m == "readonly") return true;   // outside its construction window
//       }
//     }
//
// `ModuleLoader::convertMember` sets `MemberSymbol::decl` only where it
// re-parsed body text for a GENERIC type. For an imported non-generic field
// `fs->decl` is null, so the cast never ran and the function answered "not a
// readonly field" for EVERY imported field — silently, with no diagnostic.
//
// The consequence is a DENIAL of a shape §19.3 admits, not a miscompile: the
// fall-through asks whether the RECEIVER is an admitted source, and a
// receiver reached through a class-typed hop is not (§19.3: "`this` in a
// value-kind member body is never such a source"). So an imported `readonly`
// field used as a move-only `case` scrutinee source was REJECTED where the
// identical local program is admitted.
//
// ── WHY A SAME-MODULE TEST CANNOT FIND THIS ──────────────────────────────
//
// On the local side `fs->decl` IS the `ast::FieldDecl` and the modifiers are
// right there, so the local program never exercises the defect. The
// differential only exists ACROSS the artifact boundary — which is why row T2
// below is the local twin and PASSES ON BOTH SIDES of the fix.
//
// ── THE FIX, AND WHY IT IS A REAL FIX RATHER THAN A MARKER ───────────────
//
// The `readonly` modifier already crosses the boundary:
// `ModuleMetadata.hpp:579-580` lists "readonly" in `MetaMember::modifiers`,
// `ModuleMetadataBuilder` assigns `out.modifiers = toStrings(m.modifiers)`, and
// `convertMember`'s Field case does `f->modifiers = intern(mm.modifiers)`.
// The fact was on the symbol all along and the reader was reaching past it into
// a null AST node. The fix routes the read through `ExprTC::isReadonlyField`
// (§7.4's both-sources predicate), which scans `fs->modifiers` — so one
// predicate answers on both sides, rather than two copies of one question
// reading different sources (the audit's §4 rule).
//
// `isReadonlyField` ALSO admits a positional-record field, whose auto-property
// has no setter (§9.1: a body constructor "initializes the positional storage
// by assigning the auto-generated read-only properties") and which the
// `FieldDecl` cast could never match — its `decl` is the synthetic
// `ast::Param` of #2516. Both arms only ever turn a `false` into a `true`, so
// neither can turn an admitted source into a rejected one.
//
// ── ROWS ──────────────────────────────────────────────────────────────────
//
//   T1  THE DIFFERENTIAL: an IMPORTED `readonly` field of an owning inline
//       array as a move-only `case` scrutinee source, reached through `this`
//       in the declaring class's own method. Pre-fix the app is REJECTED with
//       §19.3's "borrows its payload IN PLACE"; post-fix it compiles, runs,
//       and prints the snapshot value and the counter.
//   T2  THE LOCAL TWIN: the character-identical program with the class
//       declared in the SAME module. Admitted before the fix and after it —
//       which is what proves T1's differential is the import and not the
//       program shape.
//   T3  THE OVER-ADMISSION CONTROL: an IMPORTED field that is NOT `readonly`,
//       same program otherwise. Rejected before and after, with the same
//       diagnostic. A `isReadonlyField` that answered "readonly" to everything
//       would pass T1 and T2 and fail this.
//
// Oracle: `Driver::compileObject` (exporter) + `bezel::test::compileFileForTest`
// (importer) + native exec — the cross-module shape is invisible to the
// single-module `runAot` harness. T2 uses `runAot`.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

using bezel::test::diagnosticMessageContains;

namespace fs = std::filesystem;

const std::filesystem::path& wtDir5876() { return wtScratchDir("wt-5876"); }

struct TempFile5876 {
    std::string path;
    explicit TempFile5876(std::string p) : path(std::move(p)) {}
    ~TempFile5876() { std::remove(path.c_str()); }
};

void writeSource5876(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

std::string captureStderr5876(const std::function<void()>& body) {
    std::string errPath =
        (wtDir5876() / ("err_" + unique_stem("") + ".txt")).string();
    std::fflush(stderr);
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    body();
    std::fflush(stderr);
    if (fd >= 0) { ::close(fd); ::dup2(saved, STDERR_FILENO); }
    ::close(saved);
    std::ifstream in(errPath);
    std::string err((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return err;
}

struct XmodResult5876 {
    bool libOk = false;
    bool appOk = false;
    std::string err;
    std::string exePath;
};

XmodResult5876 compileImport5876(const std::string& libSrc,
                                 const std::string& appSrc) {
    auto dir = wtDir5876();
    std::string stem = unique_stem("bz5876_");
    XmodResult5876 out;
    TempFile5876 libSrcFile((dir / (stem + "_lib.bzl")).string());
    TempFile5876 libObjFile((dir / (stem + "_lib.o")).string());
    writeSource5876(libSrcFile.path, libSrc);
    {
        bezel::Driver driver;
        out.libOk = driver.compileObject(libSrcFile.path, libObjFile.path).success
            && std::ifstream(libObjFile.path).good();
    }
    if (!out.libOk) return out;
    TempFile5876 appSrcFile((dir / (stem + "_app.bzl")).string());
    std::string exePath = (dir / stem).string();
    writeSource5876(appSrcFile.path, appSrc);
    out.err = captureStderr5876([&] {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = libObjFile.path;
        d.objectPath = libObjFile.path;
        deps.push_back(d);
        out.appOk =
            bezel::test::compileFileForTest(driver, appSrcFile.path, exePath, deps)
                .success;
    });
    out.exePath = exePath;
    return out;
}

std::string runCapture5876(const std::string& exe) {
    std::string outPath =
        (wtDir5876() / ("out_" + unique_stem("") + ".txt")).string();
    std::string cmd = exe + " > " + outPath + " 2>&1";
    int status = std::system(cmd.c_str());
    std::ifstream in(outPath);
    std::string s((std::istreambuf_iterator<char>(in)),
                  std::istreambuf_iterator<char>());
    std::remove(outPath.c_str());
    if (status == -1 || !WIFEXITED(status)) return s;
    (void)WEXITSTATUS(status);
    return s;
}

bool contains5876(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// §19.3's own stable predicate — the substring a future reroute of this
// decision would not produce. Mirrors tests/sema/
// move_only_payload_binding_write_point_5870_test.cpp's kInPlace.
const char kInPlace5876[] = "borrows its payload IN PLACE";

// The payload cell. `owned` elements make the array move-only, which is what
// puts a §19.3 payload binding in play at all (§15.3).
std::string cellDecl5876() {
    return "class Cell5876 "
           "  public V: int := 0; "
           "  public constructor(v: int) V := v; end constructor; "
           "end Cell5876; ";
}

const char kSlotsInit5876[] =
    "Slots: [2 owned]Cell5876 := [new Cell5876(1), new Cell5876(3)]; ";

// ── LIBRARIES (T1, T2, T3) ────────────────────────────────────────────────
//
// The libraries carry ONLY the class and its field. The `case` lives in the
// IMPORTING module, which is the whole point: the §19.3 decision for an
// imported symbol is made during the IMPORTING compilation, against a
// `FieldSymbol` the loader built. A `case` written inside the library would be
// sema-checked by the library's own compilation, where `fs->decl` IS the
// `ast::FieldDecl` — so that placement tests nothing about the seam.
std::string lib5876(const std::string& fieldDecl) {
    return "module Lib5876; public " + cellDecl5876() +
           "public class Holder5876 public " + fieldDecl + " " + kSlotsInit5876 +
           "end Holder5876; ";
}

// T3's app: the `case` is HERE, over a plain BORROW parameter. The receiver's
// TYPE is the class `Holder5876`, which is the hop §19.3 names when it refuses
// "storage inside such a local's value, reached from it without leaving the
// value": a class- or interface-typed receiver is that hop, so with a mutable
// field nothing admits the source. (The receiver does not need to be a `ref`
// formal — the class-typed test is on the receiver's resolved type, not on the
// receiver symbol's stability, so an ordinary borrow parameter reaches it.)
std::string appProbe5876() {
    return "module App5876; "
           "import Lib5876; "
           "class C "
           "  public static method Probe(h: Holder5876): int "
           "    var n := 0; "
           "    case h.Slots of "
           "      var a => do "
           "        n := n + 1; "
           "        Console.WriteLine($\"{a[0].V}\"); "
           "      end "
           "    end; "
           "    Console.WriteLine($\"{n}\"); "
           "    return 0; "
           "  end Probe; "
           "  public static method Main(): int "
           "    using var h := new Holder5876(); "
           "    return C.Probe(h); "
           "  end Main; "
           "end C;";
}

}  // namespace

// ── T1 — THE DIFFERENTIAL. An IMPORTED `readonly` field of an owning inline
// array, used as a move-only `case` scrutinee source.
//
// Pre-fix (base 9ad4e6d0): `fs->decl` is null for the imported field, the
// `FieldDecl` cast never runs, the fall-through finds the receiver's type is a
// CLASS and answers "not an admitted source", and the importing compilation is
// REJECTED with §19.3's message — a denial of a shape §19.3's whitelist
// admits, and one the local twin (T2) accepts. Post-fix: admitted; the program
// runs and prints the snapshot value `1` and the counter `1`.
BEZEL_TEST(xmod_imported_readonly_field_write_point_5876,
           t1_imported_readonly_field_scrutinee_admitted_spec_19_3) {
    auto r = compileImport5876(lib5876("readonly"), appProbe5876());
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_TRUE(r.appOk);
    if (!r.appOk) return;
    BEZEL_EXPECT_EQ(runCapture5876(r.exePath), "1\n1\n");
}

// ── T2 — THE LOCAL TWIN. The character-identical program with the class
// declared in the SAME module, so `fs->decl` IS the `ast::FieldDecl` and the
// old read was already correct. Admitted before the fix and after it. This is
// what makes T1 a statement about the artifact boundary rather than about the
// program shape — and it is why a same-module test alone could never have
// caught this defect.
BEZEL_TEST(xmod_imported_readonly_field_write_point_5876,
           t2_local_twin_admitted_on_both_sides_control) {
    // T1's program with the `import` and the library replaced by the SAME
    // declarations in the SAME module — the `case` still sits over the borrow
    // parameter in `Probe`, so the only difference from T1 is the artifact
    // boundary. On this side `fs->decl` IS the `ast::FieldDecl` and the old
    // read was already correct, so this row is green BEFORE the fix too. That
    // asymmetry is the finding: a same-module test alone could never have
    // caught this defect.
    const std::string prog =
        "module Local5876; "
        "class Cell5876 "
        "  public V: int := 0; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Cell5876; "
        "class Holder5876 "
        "  public readonly Slots: [2 owned]Cell5876 := "
        "[new Cell5876(1), new Cell5876(3)]; "
        "end Holder5876; "
        "class C "
        "  public static method Probe(h: Holder5876): int "
        "    var n := 0; "
        "    case h.Slots of "
        "      var a => do "
        "        n := n + 1; "
        "        Console.WriteLine($\"{a[0].V}\"); "
        "      end "
        "    end; "
        "    Console.WriteLine($\"{n}\"); "
        "    return 0; "
        "  end Probe; "
        "  public static method Main(): int "
        "    using var h := new Holder5876(); "
        "    return C.Probe(h); "
        "  end Main; "
        "end C;";
    auto r = bezel::test::runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n1\n");
}

// ── T3 — THE OVER-ADMISSION CONTROL, ACROSS THE BOUNDARY. T1's program with
// the `readonly` marker DROPPED from the imported field. `isReadonlyField` must
// answer FALSE for the imported symbol —
// which is the assertion that matters here, because a predicate reading a null
// `decl` cannot distinguish "readonly" from "not readonly" at all and a
// predicate returning true unconditionally would pass T1 and T2.
//
// Rejected before the fix and after it, with the same diagnostic. Green on
// both sides is the point: this row is a control, not a differential.
BEZEL_TEST(xmod_imported_readonly_field_write_point_5876,
           t3_imported_mutable_field_still_rejected_control) {
    auto r = compileImport5876(lib5876(""), appProbe5876());
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(contains5876(r.err, kInPlace5876));
}

// ── T4 — THE OVER-ADMISSION CONTROL, LOCAL SIDE. The same rejection with no
// module boundary at all. T3 covers the imported symbol; this covers the local
// one, which is the path the pre-fix read already answered correctly and which
// `isReadonlyField` now serves too (it additionally admits a positional-record
// field, whose `decl` is a synthetic `ast::Param` rather than a `FieldDecl`).
BEZEL_TEST(xmod_imported_readonly_field_write_point_5876,
           t4_local_mutable_field_still_rejected_control) {
    const std::string prog =
        "module Local5876b; "
        "class Cell5876 "
        "  public V: int := 0; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Cell5876; "
        "class Holder5876 "
        "  public Slots: [2 owned]Cell5876 := "
        "[new Cell5876(1), new Cell5876(3)]; "
        "end Holder5876; "
        "class C "
        "  public static method Probe(h: Holder5876): int "
        "    var n := 0; "
        "    case h.Slots of "
        "      var a => do "
        "        n := n + 1; "
        "        Console.WriteLine($\"{a[0].V}\"); "
        "      end "
        "    end; "
        "    return 0; "
        "  end Probe; "
        "  public static method Main(): int "
        "    using var h := new Holder5876(); "
        "    return C.Probe(h); "
        "  end Main; "
        "end C;";
    auto r = bezel::test::runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kInPlace5876));
}

}  // namespace bezel::module
