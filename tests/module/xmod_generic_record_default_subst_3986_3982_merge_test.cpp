// Merge-verification test for #3986 + #3982 (§17.11, v0.294): both fixes
// touch the SAME `ExprTC::defaultIllFormed` `Record` arm
// (ExprTypecheck_Exprs.cpp) and were developed independently in sibling
// lanes:
//
//   - #3982 closed the IMPORT gap: the arm cast `t->declaration->decl` to
//     `ast::TypeDecl*` and accepted `default(T)` whenever that cast failed —
//     which it always does for an imported type (`TypeSymbol::decl` is
//     never assigned on import). Fixed by reading the SYMBOL side
//     (`recordPositionalFieldSymbols` + `storageComponentType`), which
//     answers identically for a local and an imported record.
//
//   - #3986 closed the SUBSTITUTION gap: even on the symbol side, a
//     generic record's bare-type-parameter field (`V: T` in
//     `record Box<T>(V: T)`) resolves to an unsubstituted, declaration-less
//     TypeRef, which the function's `!t->declaration` early return treats
//     as an unresolved parameter and unconditionally accepts — regardless
//     of what the CONCRETE instantiation binds `T` to. Fixed by threading a
//     `common::RecordSubstEnv` through the same walk.
//
// The two fixes are ORTHOGONAL AXES (import-or-not x generic-or-not), and
// each was tested only holding the OTHER axis fixed: #3982's own suite
// (`xmod_default_record_imported_bypass_3982`) exercises imported
// NON-generic records; #3986's own suite
// (`generic_record_default_subst_3986`) exercises LOCAL generic records.
// Neither alone proves the COMBINATION — an imported GENERIC record whose
// bare type parameter is bound to a non-nullable reference type — which is
// exactly the shape a naive three-way merge of the two diffs could pass
// both existing suites on while still being silently wrong (e.g. a merge
// that kept the AST-side `td->positionalFields` walk RE-ADDED alongside the
// symbol-side one, or one that seeded `RecordSubstEnv` from the AST-side
// `seedRecordSubstEnv(ast::TypeDecl&, ...)` overload, which cannot run at
// all once `td` is gone from the merged arm — silently reproducing #3982
// for every imported generic record).
//
// This file pins that combination directly, using the cross-module driver
// harness (mirrors xmod_default_record_imported_bypass_3982_test.cpp): the
// library declares `record BoxM3986<T>(V: T)`; the importing module asks
// for `default(BoxM3986<WidgetM3986>)`, a NON-nullable class-typed
// substitution.
//
// MEASURED on the merged arm (this fix's own commit): the combination
// REJECTS at compile time with the same §17.11 message text the all-local
// and imported-non-generic controls produce, and never reaches codegen —
// the SIGSEGV both individual defects independently produced is closed on
// this axis too.

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

const std::filesystem::path& wtDirM3986() { return wtScratchDir("wt-m3986"); }

struct TempFileM3986 {
    std::string path;
    explicit TempFileM3986(std::string p) : path(std::move(p)) {}
    ~TempFileM3986() { std::remove(path.c_str()); }
};

void writeSourceM3986(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

std::string captureStderrM3986(const std::function<void()>& body) {
    std::string errPath =
        (wtDirM3986() / ("err_" + unique_stem("") + ".txt")).string();
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

struct XmodResultM3986 {
    bool libOk = false;
    bool appOk = false;
    std::string err;
    std::string exePath;
};

XmodResultM3986 compileImportM3986(const std::string& libSrc,
                                   const std::string& appSrc) {
    auto dir = wtDirM3986();
    std::string stem = unique_stem("bzm3986_");
    XmodResultM3986 out;
    TempFileM3986 libSrcFile((dir / (stem + "_lib.bzl")).string());
    TempFileM3986 libObjFile((dir / (stem + "_lib.o")).string());
    writeSourceM3986(libSrcFile.path, libSrc);
    {
        bezel::Driver driver;
        out.libOk = driver.compileObject(libSrcFile.path, libObjFile.path).success
            && std::ifstream(libObjFile.path).good();
    }
    if (!out.libOk) return out;
    TempFileM3986 appSrcFile((dir / (stem + "_app.bzl")).string());
    std::string exePath = (dir / stem).string();
    writeSourceM3986(appSrcFile.path, appSrc);
    out.err = captureStderrM3986([&] {
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

int runCaptureM3986(const std::string& exe) {
    int status = std::system(exe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

bool containsM3986(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ── LIBRARY: a generic record whose field is a bare type parameter ────────

const char* kLibM3986 =
    "module LibM3986; "
    "public class WidgetM3986 "
    "  public Count: int; "
    "  public constructor(c: int) Count := c; end constructor; "
    "  public method Render(): int return Count; end Render; "
    "end WidgetM3986; "
    "public record BoxM3986<T>(V: T);";

// ── THE COMBINATION CELL: imported + generic + non-nullable substitution.
// Neither #3982's suite (imported, non-generic) nor #3986's suite (local,
// generic) exercises this cell. It must REJECT — the record is
// non-defaultable at THIS instantiation — and must never reach codegen. ───

BEZEL_TEST(xmod_generic_record_default_subst_3986_3982_merge,
           imported_generic_record_nonnullable_class_param_rejected) {
    const std::string app =
        "module AppM3986; "
        "import LibM3986; "
        "class ProgramM3986 "
        "  public static method Main(): int "
        "    var x := default(BoxM3986<WidgetM3986>); "
        "    return x.V.Render(); "
        "  end Main; "
        "end ProgramM3986;";
    auto r = compileImportM3986(kLibM3986, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(containsM3986(
        r.err,
        "`default(T)` is a compile error for a record any of whose fields "
        "is a non-nullable reference type"));
    BEZEL_EXPECT_TRUE(containsM3986(r.err, "(\xC2\xA7""17.11)"));
}

// ── Anti-regression control: the SAME imported generic record, substituted
// at a value type, must stay defaultable — proves the merge didn't turn
// into an unconditional reject for every imported generic record. ─────────

BEZEL_TEST(xmod_generic_record_default_subst_3986_3982_merge,
           imported_generic_record_value_param_still_accepted) {
    const std::string app =
        "module AppM3986V; "
        "import LibM3986; "
        "class ProgramM3986V "
        "  public static method Main(): int "
        "    var x := default(BoxM3986<int>); "
        "    return x.V; "
        "  end Main; "
        "end ProgramM3986V;";
    auto r = compileImportM3986(kLibM3986, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_TRUE(r.appOk);
    if (!r.appOk) return;
    BEZEL_EXPECT_EQ(runCaptureM3986(r.exePath), 0);
}

// ── Anti-regression control: the SAME imported generic record, substituted
// at a NULLABLE class type, must stay defaultable (null). ─────────────────

BEZEL_TEST(xmod_generic_record_default_subst_3986_3982_merge,
           imported_generic_record_nullable_param_still_accepted) {
    const std::string app =
        "module AppM3986N; "
        "import LibM3986; "
        "class ProgramM3986N "
        "  public static method Main(): int "
        "    var x := default(BoxM3986<WidgetM3986?>); "
        "    if x.V <> null then return 1; end if; "
        "    return 9; "
        "  end Main; "
        "end ProgramM3986N;";
    auto r = compileImportM3986(kLibM3986, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_TRUE(r.appOk);
    if (!r.appOk) return;
    BEZEL_EXPECT_EQ(runCaptureM3986(r.exePath), 9);
}

}  // namespace bezel::module
