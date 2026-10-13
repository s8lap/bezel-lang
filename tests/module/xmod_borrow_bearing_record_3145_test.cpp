// Spec-derived regression test for issue #3145 — family F10 of the
// imported-symbol classification seam (umbrella #3152).
//
// §9.7 (borrow-bearing records) / §28.3 (borrow-lifetime rule) /
// §2.8.3 rule 4 (imported artifacts), v0.252. Audit:
// docs/proposals/papers/imported-symbol-audit-2026-08-13.md, family F10.
//
// ── WHY THIS IS THE AUDIT'S HIGHEST-SEVERITY ENTRY ─────────────────────────
//
// This is not a leak: it is an ACCEPTED PROGRAM THAT THE SPEC REQUIRES BE
// REJECTED. §9.7 forbids constructing a borrow-bearing record from a temporary
// bound to its borrow-carrying field — the temporary dies at the end of the
// construction expression, so the record outlives the borrow's source. The
// reject is issued by `checkRecordFromTemporary`, which asked
//
//     dynamic_cast<const ast::TypeDecl*>(rt->declaration->decl)
//
// for the record's field list. `ModuleLoader` never assigns `Symbol::decl` for
// an imported type — there is no `ast::TypeDecl` to assign, since the importing
// compilation has no AST for a dependency's types — so that cast is null for
// EVERY import and the function returned before checking anything. The whole
// §9.7 reject silently did not exist across a module boundary. The same hole sat
// in `isBorrowBearingRecordImpl` and its #1595 lockstep partner
// `common::isBorrowBearingRecordCommon`, so the predicate they share answered
// "not borrow-bearing" for every imported record too.
//
// ── MEASURED, on this base, with a real two-module build ───────────────────
//
//   `Holder` declared LOCALLY   -> rejected, §9.7 diagnostic, exit 1
//   `Holder` IMPORTED, same use -> compiled clean, exit 0        <- the hole
//
// The two programs are otherwise identical. What differed was the module
// boundary, which is exactly what T1 and its local twin T2 pin here.
//
// ── HOW IT IS FIXED ────────────────────────────────────────────────────────
//
// The field walk moved to the symbol table, which carries the fields on BOTH
// sides: a positional field is a `FieldSymbol` locally (`posParam` -> the real
// `ast::Param`) and on import too (#2516, `posParam` -> a synthetic `ast::Param`
// with a null `type`, kept precisely so the positional/body distinction survives
// without an AST). `sema::storageComponentType` — which already consulted both
// sources but was `static` in SymbolCollection.cpp, which is why two more copies
// of the walk had grown elsewhere — is now shared, and
// `sema::recordPositionalFieldSymbols` / `sema::anyRecordStorageField` are the
// one enumeration built on it.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
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

struct TempFile3145 {
    std::string path;
    explicit TempFile3145(std::string p) : path(std::move(p)) {}
    ~TempFile3145() { std::remove(path.c_str()); }
};

void writeSource3145(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj3145(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Capture the importer's stderr. Mirrors compileImportCaptureErr2549.
bool compileImportCaptureErr3145(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz3145_err_" + unique_stem("") + ".txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj;
        d.objectPath = aObj;
        deps.push_back(d);
        success = bezel::test::compileFileForTest(driver, bSrc, bExe, deps).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) { ::close(fd); ::dup2(saved, STDERR_FILENO); }
    ::close(saved);
    std::ifstream in(errPath);
    err.assign((std::istreambuf_iterator<char>(in)),
               std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return success;
}

// `dependencyCompiled` is the ANTI-VACUITY guard: without it, a malformed lib
// source would turn the expected importer rejection into a pass for the wrong
// reason (nothing to import, so nothing to check).
struct ImportCapture3145 {
    bool dependencyCompiled = false;
    bool appCompiled = false;
    std::string error;
};

ImportCapture3145 compileImportSources3145(const std::string& libSrc,
                                           const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3145_");
    TempFile3145 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3145 aObj((dir / (stem + "_a.o")).string());
    writeSource3145(aSrc.path, libSrc);
    ImportCapture3145 out;
    out.dependencyCompiled = compileToObj3145(aSrc.path, aObj.path) &&
                             std::ifstream(aObj.path).good();
    if (!out.dependencyCompiled) return out;
    TempFile3145 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3145 bExe((dir / stem).string());
    writeSource3145(bSrc.path, appSrc);
    out.appCompiled =
        compileImportCaptureErr3145(bSrc.path, bExe.path, aObj.path, out.error);
    return out;
}

// Same-module compile, stderr captured — for the LOCAL twin.
bool compileLocalCaptureErr3145(const std::string& src, std::string& err) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3145_local_");
    std::string errPath =
        (dir / ("bz3145_local_err_" + unique_stem("") + ".txt"))
            .string();
    TempFile3145 srcFile((dir / (stem + ".bzl")).string());
    TempFile3145 exeFile((dir / stem).string());
    writeSource3145(srcFile.path, src);
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        success = bezel::test::compileFileForTest(driver, srcFile.path, exeFile.path, {}).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) { ::close(fd); ::dup2(saved, STDERR_FILENO); }
    ::close(saved);
    std::ifstream in(errPath);
    err.assign((std::istreambuf_iterator<char>(in)),
               std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return success;
}

// The §9.7 diagnostic, keyed on wording stable enough to identify it and
// specific enough that another reject cannot satisfy the assertion.
bool saysDanglingBorrowReject3145(const std::string& err) {
    return err.find("a borrow-bearing record may not be constructed from a "
                    "temporary") != std::string::npos &&
           err.find("dangling borrow") != std::string::npos;
}

const char* kLib =
    "module DepF10_3145; "
    "public class Res3145 "
    "  public v: int := 0; "
    "  public constructor(x: int) v := x; end constructor; "
    "end Res3145; "
    "public record Holder3145(r: Res3145, tag: int); "
    "public record Plain3145(a: int, b: int); ";

}  // namespace

// ===========================================================================
// T1 — FAILS WITHOUT THE FIX. `new Holder3145(new Res3145(1), 7)` on an
// IMPORTED borrow-bearing record. Pre-fix this compiled clean (exit 0): the
// field list was read through `->decl`, which is null for an import, so
// checkRecordFromTemporary returned before inspecting a single argument.
// // per §9.7 / §28.3 / §2.8.3 rule 4, v0.252
// ===========================================================================
BEZEL_TEST(xmod_borrow_bearing_record_3145,
           imported_record_from_temporary_is_rejected) {
    auto r = compileImportSources3145(
        kLib,
        "module AppF10_3145a; import DepF10_3145; "
        "public class C "
        "  public static method Main(): int "
        "    var h := new Holder3145(new Res3145(1), 7); "
        "    return h.tag; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);   // anti-vacuity
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(saysDanglingBorrowReject3145(r.error));
    // The diagnostic must name the borrow-carrying field, not the `int`
    // sibling — the symbol-side positional list must stay index-aligned.
    BEZEL_EXPECT_TRUE(r.error.find("`r`") != std::string::npos);
}

// ===========================================================================
// ★ T2 — THE DIFFERENTIAL CONTROL. The identical program with the record
// declared in the SAME module. It rejected before the fix and must still
// reject: it is what pins that the module boundary was the entire difference,
// and that moving the walk to the symbol table did not change the local answer.
// // per §9.7 / §28.3
// ===========================================================================
BEZEL_TEST(xmod_borrow_bearing_record_3145,
           local_record_from_temporary_still_rejected_control) {
    std::string err;
    bool ok = compileLocalCaptureErr3145(
        "module LocalF10_3145; "
        "public class Res3145L "
        "  public v: int := 0; "
        "  public constructor(x: int) v := x; end constructor; "
        "end Res3145L; "
        "public record Holder3145L(r: Res3145L, tag: int); "
        "public class C "
        "  public static method Main(): int "
        "    var h := new Holder3145L(new Res3145L(1), 7); "
        "    return h.tag; "
        "  end Main; "
        "end C;", err);
    BEZEL_EXPECT_FALSE(ok);
    BEZEL_EXPECT_TRUE(saysDanglingBorrowReject3145(err));
}

// ===========================================================================
// ★ T3 — THE FALSE-REJECT CONTROL, and the reason this file is not simply
// "everything fails now". A record whose fields are all VALUE types is not
// borrow-bearing, so constructing it from temporaries is fine. If the symbol-
// side walk were over-broad — treating any imported record as borrow-bearing
// because it could not see the field types — this would go red.
// // per §9.7
// ===========================================================================
BEZEL_TEST(xmod_borrow_bearing_record_3145,
           imported_value_record_from_temporary_still_accepted) {
    auto r = compileImportSources3145(
        kLib,
        "module AppF10_3145b; import DepF10_3145; "
        "public class C "
        "  public static method Main(): int "
        "    var p := new Plain3145(3, 4); "
        "    return p.a + p.b; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);
    BEZEL_EXPECT_TRUE(r.appCompiled);
    BEZEL_EXPECT_FALSE(saysDanglingBorrowReject3145(r.error));
}

// ===========================================================================
// T4 — the §9.7 ACCEPT the rule names: bind the source to a NAMED LOCAL whose
// scope outlives the record and pass that. Pins that the fix rejects the
// temporary specifically, not the borrow-bearing construction as such — the
// remedy the diagnostic recommends must actually work across a module
// boundary. // per §9.7 / §28.3
// ===========================================================================
BEZEL_TEST(xmod_borrow_bearing_record_3145,
           imported_record_from_named_local_is_accepted) {
    auto r = compileImportSources3145(
        kLib,
        "module AppF10_3145c; import DepF10_3145; "
        "public class C "
        "  public static method Main(): int "
        "    var src := new Res3145(1); "
        "    var h := new Holder3145(src, 7); "
        "    return h.tag; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);
    BEZEL_EXPECT_TRUE(r.appCompiled);
    BEZEL_EXPECT_FALSE(saysDanglingBorrowReject3145(r.error));
}

}  // namespace bezel::module
