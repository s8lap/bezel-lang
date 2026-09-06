// fix #3982 — §17.11 residual (v0.294): `default(T)` over an IMPORTED
// (cross-module) record with a non-nullable reference-typed field silently
// ACCEPTED and then SIGSEGV'd at runtime.
//
// ── ROOT CAUSE ───────────────────────────────────────────────────────────
//
// `ExprTC::defaultIllFormed`'s `Record` arm (ExprTypecheck_Exprs.cpp) used
// to cast `t->declaration->decl` to `const ast::TypeDecl*` and walk ITS
// `positionalFields`, returning "well-formed" (accept, i.e. `default(T)` is
// legal) whenever that cast failed. `TypeSymbol::decl` is NEVER assigned for
// an imported type (sema/Symbol.hpp; the identical fact fixes #3911, #3957
// and #3970 each independently hit for sibling AST-side reads in this same
// family), so the cast fails for EVERY imported record and the function
// silently answered "well-formed" regardless of what the record's fields
// actually held — even a non-nullable class-typed field, which §17.11 says
// makes the record's `default(T)` a compile error.
//
// Unlike #3970's sibling gap (a pure static-checking soundness hole — that
// fix's codegen path was independently import-safe), this one is a REAL
// SIGSEGV: codegen's `default(T)` lowering for a record recursively
// default-constructs each field, so a non-nullable class-typed field's
// "default" is a null pointer with no diagnostic having ever fired, and a
// subsequent member access null-derefs.
//
// The fix reads the SYMBOL side instead — `recordPositionalFieldSymbols` +
// `storageComponentType` (sema/Symbol.hpp, "the ONE accessor that consults
// both sources") — which answers identically for a local and an imported
// record, mirroring #3970's own record-field-walk fix in the same file
// family.
//
// ── MEASURED, on this fix's parent (imported vs. local twin) ───────────────
//
//   Imported record with a non-nullable class field: pre-fix ZERO
//   diagnostic, exit 0 at compile time, exit 139 (SIGSEGV) at runtime.
//   Local twin: clean §17.11 rejection, exit 1, never reaches codegen.
//
// Post-fix the imported case rejects with the SAME §17.11 message text the
// local twin produces.
//
// ── SWEEP (not fixed here; separate, non-import root cause) ────────────────
//
// A generic record instantiation (`record BoxG<T>(V: T)`, `default(BoxG<W>)`
// for a non-nullable class `W`) reproduces an IDENTICAL SIGSEGV for a purely
// LOCAL declaration with no import involved at all: `defaultIllFormed`'s
// Record arm never substitutes the record's own generic type parameter
// against the concrete instantiation argument, so a bare-type-parameter
// field is always treated as "unresolved generic parameter" (line ~403,
// `!t->declaration` -> false/well-formed) regardless of what the concrete
// instantiation binds it to. This is filed as a separate issue (a
// generic-substitution gap, not an import/AST-cast gap) and is NOT covered
// by this fix or this test file.
//
// ── ORACLE CHOICE ────────────────────────────────────────────────────────
//
// AOT execution via `Driver::compileObject` (library) +
// `bezel::test::compileFileForTest` (importer) is the oracle for every
// cross-module case, mirroring
// xmod_static_readonly_null_source_imported_record_bypass_3970_test.cpp; the
// single-module local-twin control uses `runAot` +
// `failedAtCompileTimeWithRealLocation` (tests/support/aot_result_asserts.hpp)
// so the assertion cannot be satisfied by a runtime crash or an ASan report
// — only a genuine compile-time rejection with a real diagnostic location.

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

namespace fs = std::filesystem;

const std::filesystem::path& wtDir3982() { return wtScratchDir("wt-3982"); }

struct TempFile3982 {
    std::string path;
    explicit TempFile3982(std::string p) : path(std::move(p)) {}
    ~TempFile3982() { std::remove(path.c_str()); }
};

void writeSource3982(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

std::string captureStderr3982(const std::function<void()>& body) {
    std::string errPath =
        (wtDir3982() / ("err_" + unique_stem("") + ".txt")).string();
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

struct XmodResult3982 {
    bool libOk = false;
    bool appOk = false;
    std::string err;
    std::string exePath;
};

XmodResult3982 compileImport3982(const std::string& libSrc,
                                 const std::string& appSrc) {
    auto dir = wtDir3982();
    std::string stem = unique_stem("bz3982_");
    XmodResult3982 out;
    TempFile3982 libSrcFile((dir / (stem + "_lib.bzl")).string());
    TempFile3982 libObjFile((dir / (stem + "_lib.o")).string());
    writeSource3982(libSrcFile.path, libSrc);
    {
        bezel::Driver driver;
        out.libOk = driver.compileObject(libSrcFile.path, libObjFile.path).success
            && std::ifstream(libObjFile.path).good();
    }
    if (!out.libOk) return out;
    TempFile3982 appSrcFile((dir / (stem + "_app.bzl")).string());
    std::string exePath = (dir / stem).string();
    writeSource3982(appSrcFile.path, appSrc);
    out.err = captureStderr3982([&] {
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

int runCapture3982(const std::string& exe) {
    int status = std::system(exe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

bool contains3982(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ── LIBRARY ──────────────────────────────────────────────────────────────

const char* kLib3982 =
    "module Lib3982; "
    "public class Widget3982 "
    "  public Count: int; "
    "  public constructor(c: int) Count := c; end constructor; "
    "  public method Render(): int return Count; end Render; "
    "end Widget3982; "
    "public record Tok3982(Src: Widget3982); "
    "public record Tok3982Nullable(Src: Widget3982?); "
    "public record Inner3982(Src: Widget3982); "
    "public record Outer3982(In: Inner3982, Tag: int); "
    "public record Plain3982(X: int, Y: int);";

// ── (1) the primary repro: `default(T)` over an imported record with a
//        non-nullable class-typed field must REJECT at compile time, never
//        reach codegen, and never SIGSEGV ───────────────────────────────
BEZEL_TEST(xmod_default_record_imported_bypass_3982,
           class_typed_field_imported_record_rejected_spec_17_11) {
    const std::string app =
        "module App3982; "
        "import Lib3982; "
        "class Program3982 "
        "  public static method Main(): int "
        "    var x := default(Tok3982); "
        "    return x.Src.Render(); "
        "  end Main; "
        "end Program3982;";
    auto r = compileImport3982(kLib3982, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(contains3982(
        r.err,
        "`default(T)` is a compile error for a record any of whose fields "
        "is a non-nullable reference type"));
    BEZEL_EXPECT_TRUE(contains3982(r.err, "(§17.11)"));
}

// ── (1, local twin) same shape declared LOCALLY — anti-regression: must
//        keep rejecting exactly as before, with a REAL compile-time
//        location (not a bare `!ran`, which a runtime crash also
//        satisfies) ────────────────────────────────────────────────────
BEZEL_TEST(xmod_default_record_imported_bypass_3982,
           class_typed_field_local_twin_still_rejects_spec_17_11) {
    const char* prog =
        "module AppLocal3982; "
        "class Widget3982Local "
        "  public Count: int; "
        "  public constructor(c: int) Count := c; end constructor; "
        "  public method Render(): int return Count; end Render; "
        "end Widget3982Local; "
        "record Tok3982Local(Src: Widget3982Local); "
        "class Program3982Local "
        "  public static method Main(): int "
        "    var x := default(Tok3982Local); "
        "    return x.Src.Render(); "
        "  end Main; "
        "end Program3982Local;";
    auto r = bezel::test::runAot(prog);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    bool saw = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            contains3982(d.message,
                        "`default(T)` is a compile error for a record any "
                        "of whose fields is a non-nullable reference type"))
            saw = true;
    }
    BEZEL_EXPECT_TRUE(saw);
}

// ── (2, positive control) a nullable reference-typed field is a valid
//        null source — must keep compiling AND running, with the value
//        actually observed at runtime (not just "compiled") ──────────────
BEZEL_TEST(xmod_default_record_imported_bypass_3982,
           nullable_field_imported_record_positive_control) {
    const std::string app =
        "module App3982Pos; "
        "import Lib3982; "
        "class Program3982Pos "
        "  public static method Main(): int "
        "    var x := default(Tok3982Nullable); "
        "    if x.Src <> null then return 1; end if; "
        "    return 7; "
        "  end Main; "
        "end Program3982Pos;";
    auto r = compileImport3982(kLib3982, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_TRUE(r.appOk);
    if (!r.appOk) return;
    BEZEL_EXPECT_EQ(runCapture3982(r.exePath), 7);
}

// ── (3) nested imported records — the recursive per-field walk must
//        reject through TWO levels of imported record, proving the fix
//        generalises (both levels are symbol-side, no AST needed at
//        either) ───────────────────────────────────────────────────────
BEZEL_TEST(xmod_default_record_imported_bypass_3982,
           nested_record_imported_rejected_spec_17_11) {
    const std::string app =
        "module App3982Nested; "
        "import Lib3982; "
        "class Program3982Nested "
        "  public static method Main(): int "
        "    var x := default(Outer3982); "
        "    return x.Tag; "
        "  end Main; "
        "end Program3982Nested;";
    auto r = compileImport3982(kLib3982, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(contains3982(
        r.err,
        "`default(T)` is a compile error for a record any of whose fields "
        "is a non-nullable reference type"));
}

// ── (4, anti-regression) a genuinely value-like imported record (all
//        primitive fields) must keep being DEFAULTABLE — the fix must not
//        over-reject a record with no reference-typed field at all ───────
BEZEL_TEST(xmod_default_record_imported_bypass_3982,
           value_like_imported_record_still_defaultable_spec_17_11) {
    const std::string app =
        "module App3982Value; "
        "import Lib3982; "
        "class Program3982Value "
        "  public static method Main(): int "
        "    var x := default(Plain3982); "
        "    return x.X + x.Y; "
        "  end Main; "
        "end Program3982Value;";
    auto r = compileImport3982(kLib3982, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_TRUE(r.appOk);
    if (!r.appOk) return;
    BEZEL_EXPECT_EQ(runCapture3982(r.exePath), 0);
}

}  // namespace bezel::module
