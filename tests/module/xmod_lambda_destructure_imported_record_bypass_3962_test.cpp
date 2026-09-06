// fix #3962 — §27.4 / §7 / §9.1 / §2.8.3 rule 4 (v0.294): an IMPORTED
// (cross-module) record's §27.4 lambda-parameter destructuring silently lost
// its field type, disabling the §7 nullable-mismatch check on the bound
// local.
//
// ── ROOT CAUSE ───────────────────────────────────────────────────────────
//
// `ExprTC::bindLambdaDestructure` (ExprTypecheck_Exprs.cpp) already resolves
// the pattern head's record `TypeSymbol` import-safely (fix #2844 added the
// `ctx_.importedSymbols()` fallback after the local searches miss). But the
// PER-SLOT field-type read at each destructured binding used the bare
// `fields[i]->posParam->type`, which fix #2516 documents as DELIBERATELY
// null for an imported record's positional field (the real type lives in
// `ctx_.importedMemberType`, reached only through the shared
// `storageComponentType` accessor, sema/Symbol.hpp — the "ONE accessor that
// consults both sources"). So every lambda-destructured slot over an
// IMPORTED record bound its local with `declType = nullptr` — not merely
// "unknown type" but a value that turns off every check keyed on it, the
// §7 nullable-argument-mismatch check included.
//
// This is the lambda-parameter sibling of fix #3957 (record `case`
// destructuring) and fix #3970 (static-readonly null-source enforcement) —
// same root defect (a bare `posParam->type` read where `storageComponentType`
// is the documented authority), a third independent call site.
//
// ── MEASURED, on this fix's parent, imported vs. local twin ────────────────
//
//   `X: int?` destructured, passed to a non-nullable `int` parameter:
//     imported -> compiles clean, exit 0 (the nullable value silently
//                 reaches a call site typed as guaranteed non-null)
//     local    -> §7 rejection, exit 1
//
// codegen's own field-type lookup (`bindLambdaDestructureParam`,
// LlvmBackend_Exprs2.cpp, fix #2844) is independently import-aware and
// computes the CORRECT runtime value regardless — this is a pure
// static-checking soundness gap, not a memory-safety bug (same severity
// class as #3957/#3970's own findings).
//
// ── ORACLE CHOICE ────────────────────────────────────────────────────────
//
// AOT execution via `Driver::compileObject` (library) +
// `bezel::test::compileFileForTest` (importer) is the oracle for every
// cross-module case; the single-module local-twin and anti-regression
// controls use `runAot` directly (mirrors
// xmod_case_destructure_imported_record_bypass_3957_test.cpp /
// xmod_static_readonly_null_source_imported_record_bypass_3970_test.cpp).

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
#include "support/aot_support.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

const std::filesystem::path& wtDir3962() { return wtScratchDir("wt-3962"); }

struct TempFile3962 {
    std::string path;
    explicit TempFile3962(std::string p) : path(std::move(p)) {}
    ~TempFile3962() { std::remove(path.c_str()); }
};

void writeSource3962(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

std::string captureStderr3962(const std::function<void()>& body) {
    std::string errPath =
        (wtDir3962() / ("err_" + unique_stem("") + ".txt")).string();
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

struct XmodResult3962 {
    bool libOk = false;
    bool appOk = false;
    std::string err;
    std::string exePath;
};

XmodResult3962 compileImport3962(const std::string& libSrc,
                                 const std::string& appSrc) {
    auto dir = wtDir3962();
    std::string stem = unique_stem("bz3962_");
    XmodResult3962 out;
    TempFile3962 libSrcFile((dir / (stem + "_lib.bzl")).string());
    TempFile3962 libObjFile((dir / (stem + "_lib.o")).string());
    writeSource3962(libSrcFile.path, libSrc);
    {
        bezel::Driver driver;
        out.libOk = driver.compileObject(libSrcFile.path, libObjFile.path).success
            && std::ifstream(libObjFile.path).good();
    }
    if (!out.libOk) return out;
    TempFile3962 appSrcFile((dir / (stem + "_app.bzl")).string());
    std::string exePath = (dir / stem).string();
    writeSource3962(appSrcFile.path, appSrc);
    out.err = captureStderr3962([&] {
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

int runCapture3962(const std::string& exe) {
    int status = std::system(exe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

bool contains3962(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ── LIBRARIES ────────────────────────────────────────────────────────────

const char* kLibPt3962 = "module Lib3962; public record Pt3962(X: int?, Y: int);";

// ── (a) headline shape — nullable field destructured, passed to a
//        non-nullable parameter: rejected when imported, same as local ────
BEZEL_TEST(xmod_lambda_destructure_imported_record_bypass_3962,
           nullable_field_imported_record_rejected_spec_7_27_4) {
    const std::string app =
        "module App3962; "
        "import Lib3962; "
        "class Helper3962 "
        "  public static method TakeInt(v: int): int return v; end TakeInt; "
        "end Helper3962; "
        "class C3962 public static method Main(): int "
        "  var p := new Pt3962(3, 4); "
        "  var f: (Pt3962) -> int := lambda(Pt3962(x, y)) => Helper3962.TakeInt(x); "
        "  return f(p); "
        "end Main; end C3962;";
    auto r = compileImport3962(kLibPt3962, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(contains3962(
        r.err, "is nullable but the target type is non-nullable (§7)"));
}

// ── (a, local twin) same shape declared LOCALLY — anti-regression ─────────
BEZEL_TEST(xmod_lambda_destructure_imported_record_bypass_3962,
           nullable_field_local_twin_still_rejects_spec_7_27_4) {
    const char* prog =
        "module AppLocal3962; "
        "record Pt3962Local(X: int?, Y: int); "
        "class Helper3962Local "
        "  public static method TakeInt(v: int): int return v; end TakeInt; "
        "end Helper3962Local; "
        "class C3962Local public static method Main(): int "
        "  var p := new Pt3962Local(3, 4); "
        "  var f: (Pt3962Local) -> int := "
        "    lambda(Pt3962Local(x, y)) => Helper3962Local.TakeInt(x); "
        "  return f(p); "
        "end Main; end C3962Local;";
    auto r = bezel::test::runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    bool saw = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            contains3962(d.message,
                        "is nullable but the target type is non-nullable (§7)"))
            saw = true;
    }
    BEZEL_EXPECT_TRUE(saw);
}

// ── (b) positive control — the non-nullable field of the same imported
//        record, destructured and used correctly, must keep compiling AND
//        running to the right value ────────────────────────────────────────
BEZEL_TEST(xmod_lambda_destructure_imported_record_bypass_3962,
           non_nullable_field_imported_positive_control_spec_27_4) {
    const std::string app =
        "module App3962Pos; "
        "import Lib3962; "
        "class Helper3962Pos "
        "  public static method TakeInt(v: int): int return v; end TakeInt; "
        "end Helper3962Pos; "
        "class C3962Pos public static method Main(): int "
        "  var p := new Pt3962(3, 4); "
        "  var f: (Pt3962) -> int := lambda(Pt3962(x, y)) => Helper3962Pos.TakeInt(y); "
        "  return f(p); "
        "end Main; end C3962Pos;";
    auto r = compileImport3962(kLibPt3962, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_TRUE(r.appOk);
    if (!r.appOk) return;
    BEZEL_EXPECT_EQ(runCapture3962(r.exePath), 4);
}

// ── (c) positive control — the nullable field used in a NULLABLE-typed
//        position (dereferenced with `!`), imported: must compile and run
//        to the right value, proving the fix does not over-reject a
//        legitimate nullable use ──────────────────────────────────────────
BEZEL_TEST(xmod_lambda_destructure_imported_record_bypass_3962,
           nullable_field_correct_use_imported_positive_control_spec_27_4) {
    const std::string app =
        "module App3962Pos2; "
        "import Lib3962; "
        "class Helper3962Pos2 "
        "  public static method TakeInt(v: int): int return v; end TakeInt; "
        "end Helper3962Pos2; "
        "class C3962Pos2 public static method Main(): int "
        "  var p := new Pt3962(7, 4); "
        "  var f: (Pt3962) -> int := "
        "    lambda(Pt3962(x, y)) => "
        "      (if x <> null then Helper3962Pos2.TakeInt(x!) else 0); "
        "  return f(p); "
        "end Main; end C3962Pos2;";
    auto r = compileImport3962(kLibPt3962, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_TRUE(r.appOk);
    if (!r.appOk) return;
    BEZEL_EXPECT_EQ(runCapture3962(r.exePath), 7);
}

}  // namespace bezel::module
