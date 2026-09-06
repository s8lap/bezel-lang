// fix #3970 — §9.7 / §22.12 rule 4 residual (v0.265, #3273/#3290), v0.294:
// an IMPORTED (cross-module) record's static readonly §9.7 null-source
// enforcement was silently bypassed.
//
// ── ROOT CAUSE (two independent, stacked gaps) ─────────────────────────────
//
// `checkStaticReadonlyNullSource`'s enforcement gate consults
// `isBorrowBearingRecordForStaticReadonly` (TypeResolution_Checks.cpp) to
// decide whether a static readonly field/property's initializer is subject
// to §9.7's null-source check. That predicate re-implemented a record-field
// walk with a bare `fs->posParam->type` read (plus an `fs->decl`-as-
// `ast::FieldDecl` AST fallback, also structurally local-only per #3152)
// instead of routing through `sema::storageComponentType` — the "ONE
// accessor that consults both sources" per its own doc comment
// (sema/Symbol.hpp). For an IMPORTED record's positional field, fix #2516
// documents `posParam->type` as DELIBERATELY null, so the predicate answered
// "not borrow-bearing" for every field of every imported record, and the
// `deferred` gate it feeds never fired — the whole §9.7 null-source
// enforcement pass never ran for a static readonly field of ANY imported
// borrow-bearing record type.
//
// Fixing only that predicate is NOT sufficient: `checkStaticReadonlyRecordInit`
// — the function that actually walks a `new Tok(...)` initializer's
// constructor arguments and emits the rejection — separately cast
// `declTy->declaration->decl` to `const ast::TypeDecl*` and returned
// immediately ("accept conservatively") when that cast failed.
// `TypeSymbol::decl` is NEVER assigned for an imported type (Symbol.hpp;
// fix #3145's OwnershipFacts.cpp comment states the identical fact for the
// sibling field-walk predicate), so this second, INDEPENDENT gap silently
// admitted every imported record/choice's static readonly initializer
// EVEN AFTER the `deferred` gate above is fixed to correctly say "yes,
// enforce this" (measured: fixing only the first predicate left the
// question below still fully unenforced). The fix reads the SYMBOL side
// instead — `recordPositionalFieldSymbols`/`storageComponentType` for a
// record's positional-field shape (mirroring the sibling predicate's own
// fix), and `choiceCasesOf`/`ChoiceCaseSymbol::caseDecl` for a choice's case
// shape (whose payload `ast::Param`s, unlike a record's positional
// `posParam`, are reconstructed with a REAL non-null `type` node by fix
// #3144's `ModuleLoader::convertChoiceCases`, so only the `td` gate in front
// of them needed removing).
//
// ── MEASURED, on this fix's parent, imported vs. local twin ────────────────
//
//   (A) Class-typed field (Tok2(Src: Widget?)) — pre-fix: ZERO diagnostic
//       of any kind, exit 0. Local twin: §9.7 rejection, exit 1.
//   (B) Object?-typed field (Tok(Src: Object?)) — pre-fix: the WRONG
//       declaration-site warning ("reachable from every domain", exit 0,
//       not a rejection). Local twin: §9.7 rejection, exit 1.
//
// Post-fix both (A) and (B), imported, reject with the SAME §9.7 message
// text the local twin produces.
//
// ── ORACLE CHOICE ────────────────────────────────────────────────────────
//
// AOT execution via `Driver::compileObject` (library) +
// `bezel::test::compileFileForTest` (importer) is the oracle for every
// cross-module case; the single-module local-twin and anti-regression
// controls use `runAot` directly (mirrors
// xmod_case_destructure_imported_record_bypass_3957_test.cpp).

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

const std::filesystem::path& wtDir3970() { return wtScratchDir("wt-3970"); }

struct TempFile3970 {
    std::string path;
    explicit TempFile3970(std::string p) : path(std::move(p)) {}
    ~TempFile3970() { std::remove(path.c_str()); }
};

void writeSource3970(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

std::string captureStderr3970(const std::function<void()>& body) {
    std::string errPath =
        (wtDir3970() / ("err_" + unique_stem("") + ".txt")).string();
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

struct XmodResult3970 {
    bool libOk = false;
    bool appOk = false;
    std::string err;
    std::string exePath;
};

XmodResult3970 compileImport3970(const std::string& libSrc,
                                 const std::string& appSrc) {
    auto dir = wtDir3970();
    std::string stem = unique_stem("bz3970_");
    XmodResult3970 out;
    TempFile3970 libSrcFile((dir / (stem + "_lib.bzl")).string());
    TempFile3970 libObjFile((dir / (stem + "_lib.o")).string());
    writeSource3970(libSrcFile.path, libSrc);
    {
        bezel::Driver driver;
        out.libOk = driver.compileObject(libSrcFile.path, libObjFile.path).success
            && std::ifstream(libObjFile.path).good();
    }
    if (!out.libOk) return out;
    TempFile3970 appSrcFile((dir / (stem + "_app.bzl")).string());
    std::string exePath = (dir / stem).string();
    writeSource3970(appSrcFile.path, appSrc);
    out.err = captureStderr3970([&] {
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

int runCapture3970(const std::string& exe) {
    int status = std::system(exe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

bool contains3970(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ── LIBRARIES ────────────────────────────────────────────────────────────

const char* kLibClassField3970 =
    "module Lib3970A; "
    "public class Widget3970 "
    "  public method Render(): int return 42; end Render; "
    "end Widget3970; "
    "public record Tok3970A(Src: Widget3970?);";

const char* kLibObjectField3970 =
    "module Lib3970B; "
    "public class Widget3970B "
    "  public method Render(): int return 42; end Render; "
    "end Widget3970B; "
    "public record Tok3970B(Src: Object?);";

// ── (A) Class-typed positional field — the severe case: ZERO diagnostic
//        pre-fix, not even a warning ────────────────────────────────────
BEZEL_TEST(xmod_static_readonly_null_source_imported_record_bypass_3970,
           class_typed_field_imported_record_rejected_spec_9_7) {
    const std::string app =
        "module App3970A; "
        "import Lib3970A; "
        "class Holder3970A "
        "  public static readonly F: Tok3970A := new Tok3970A(new Widget3970()); "
        "end Holder3970A; "
        "class Program3970A public static method Main(): int return 0; "
        "end Main; end Program3970A;";
    auto r = compileImport3970(kLibClassField3970, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(contains3970(
        r.err,
        "has record value with non-null reference-typed field `Src`"));
    BEZEL_EXPECT_TRUE(contains3970(r.err, "§9.7 requires the null-source form"));
}

// ── (A, local twin) same shape declared LOCALLY — anti-regression: this
//        must keep rejecting exactly as before ────────────────────────────
BEZEL_TEST(xmod_static_readonly_null_source_imported_record_bypass_3970,
           class_typed_field_local_twin_still_rejects_spec_9_7) {
    const char* prog =
        "module AppLocal3970A; "
        "class Widget3970ALocal "
        "  public method Render(): int return 42; end Render; "
        "end Widget3970ALocal; "
        "record Tok3970ALocal(Src: Widget3970ALocal?); "
        "class Holder3970ALocal "
        "  public static readonly F: Tok3970ALocal := "
        "    new Tok3970ALocal(new Widget3970ALocal()); "
        "end Holder3970ALocal; "
        "class Program3970ALocal public static method Main(): int return 0; "
        "end Main; end Program3970ALocal;";
    auto r = bezel::test::runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    bool saw = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            contains3970(d.message,
                        "has record value with non-null reference-typed field `Src`"))
            saw = true;
    }
    BEZEL_EXPECT_TRUE(saw);
}

// ── (A, positive control) null-source imported record — must keep
//        compiling AND running to the right value ─────────────────────────
BEZEL_TEST(xmod_static_readonly_null_source_imported_record_bypass_3970,
           class_typed_field_null_source_imported_positive_control) {
    const std::string app =
        "module App3970APos; "
        "import Lib3970A; "
        "class Holder3970APos "
        "  public static readonly F: Tok3970A := new Tok3970A(null); "
        "end Holder3970APos; "
        "class Program3970APos public static method Main(): int return 11; "
        "end Main; end Program3970APos;";
    auto r = compileImport3970(kLibClassField3970, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_TRUE(r.appOk);
    if (!r.appOk) return;
    BEZEL_EXPECT_EQ(runCapture3970(r.exePath), 11);
}

// ── (B) Object?-typed positional field — the narrower edge case: pre-fix
//        drew the WRONG declaration-site warning ON TOP OF the same
//        enforcement bypass ────────────────────────────────────────────
BEZEL_TEST(xmod_static_readonly_null_source_imported_record_bypass_3970,
           object_typed_field_imported_record_rejected_spec_9_7) {
    const std::string app =
        "module App3970B; "
        "import Lib3970B; "
        "class Holder3970B "
        "  public static readonly F: Tok3970B := new Tok3970B(new Widget3970B()); "
        "end Holder3970B; "
        "class Program3970B public static method Main(): int return 0; "
        "end Main; end Program3970B;";
    auto r = compileImport3970(kLibObjectField3970, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(contains3970(
        r.err,
        "has record value with non-null reference-typed field `Src`"));
    // The pre-fix WRONG diagnostic must be gone, not merely superseded.
    BEZEL_EXPECT_FALSE(contains3970(r.err, "reachable from every domain"));
}

// ── (B, local twin) ────────────────────────────────────────────────────
BEZEL_TEST(xmod_static_readonly_null_source_imported_record_bypass_3970,
           object_typed_field_local_twin_still_rejects_spec_9_7) {
    const char* prog =
        "module AppLocal3970B; "
        "class Widget3970BLocal "
        "  public method Render(): int return 42; end Render; "
        "end Widget3970BLocal; "
        "record Tok3970BLocal(Src: Object?); "
        "class Holder3970BLocal "
        "  public static readonly F: Tok3970BLocal := "
        "    new Tok3970BLocal(new Widget3970BLocal()); "
        "end Holder3970BLocal; "
        "class Program3970BLocal public static method Main(): int return 0; "
        "end Main; end Program3970BLocal;";
    auto r = bezel::test::runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    bool saw = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            contains3970(d.message,
                        "has record value with non-null reference-typed field `Src`"))
            saw = true;
    }
    BEZEL_EXPECT_TRUE(saw);
}

// ── (B, positive control) null-source imported record with an Object?
//        field — must keep compiling AND running ──────────────────────────
BEZEL_TEST(xmod_static_readonly_null_source_imported_record_bypass_3970,
           object_typed_field_null_source_imported_positive_control) {
    const std::string app =
        "module App3970BPos; "
        "import Lib3970B; "
        "class Holder3970BPos "
        "  public static readonly F: Tok3970B := new Tok3970B(null); "
        "end Holder3970BPos; "
        "class Program3970BPos public static method Main(): int return 12; "
        "end Main; end Program3970BPos;";
    auto r = compileImport3970(kLibObjectField3970, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_TRUE(r.appOk);
    if (!r.appOk) return;
    BEZEL_EXPECT_EQ(runCapture3970(r.exePath), 12);
}

// ── (C, anti-regression) a NON-borrow-bearing imported record's static
//        readonly field must keep taking the default rule-4 WARNING (not
//        an error, not silence) — proves the fix does not over-widen the
//        `deferred` classification for a genuinely value-like record ──────
BEZEL_TEST(xmod_static_readonly_null_source_imported_record_bypass_3970,
           value_like_imported_record_still_warns_not_errors_spec_22_12) {
    const char* lib =
        "module Lib3970C; public record Plain3970C(X: int, Y: int);";
    const std::string app =
        "module App3970C; "
        "import Lib3970C; "
        "class Holder3970C "
        "  public static readonly F: Plain3970C := new Plain3970C(1, 2); "
        "end Holder3970C; "
        "class Program3970C public static method Main(): int return 0; "
        "end Main; end Program3970C;";
    auto r = compileImport3970(lib, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    // A warning, not a rejection: the program still compiles.
    BEZEL_EXPECT_TRUE(r.appOk);
}

}  // namespace bezel::module
