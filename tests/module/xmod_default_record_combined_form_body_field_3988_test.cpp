// fix #3988 (§17.11/§9.3, v0.294): the imported-module twin of
// tests/sema/default_record_combined_form_body_field_3988_test.cpp.
//
// `ExprTC::defaultIllFormed`'s Record arm walked only `td->positionalFields`
// off the AST, and `td` came from `dynamic_cast<const ast::TypeDecl*>
// (t->declaration->decl)` — always null for an imported type (`TypeSymbol::
// decl` is never assigned across a module boundary), so the walk deferred
// (silently accepted `default(T)`) for EVERY imported record regardless of
// its fields, compounding the body-field gap this issue is about. The fix
// (see the local-spelling test file for the full root-cause writeup) routes
// through `recordStorageMembers` (sema/Symbol.hpp), which is symbol-side and
// answers identically for a local and an imported record.
//
// MEASURED on this fix's parent: an imported combined-form record with a
// non-nullable class-typed body field compiled clean (exit 0) and SIGSEGV'd
// (exit 139) at runtime — identically to the local spelling.
//
// ORACLE: AOT execution via `Driver::compileObject` (library) +
// `bezel::test::compileFileForTest` (importer), mirroring
// xmod_case_destructure_imported_record_bypass_3957_test.cpp — the two-
// module cross-compilation-unit harness this family of fixes uses.

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
#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

const std::filesystem::path& wtDir3988() { return wtScratchDir("wt-3988"); }

struct TempFile3988 {
    std::string path;
    explicit TempFile3988(std::string p) : path(std::move(p)) {}
    ~TempFile3988() { std::remove(path.c_str()); }
};

void writeSource3988(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

std::string captureStderr3988(const std::function<void()>& body) {
    std::string errPath =
        (wtDir3988() / ("err_" + unique_stem("") + ".txt")).string();
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

struct XmodResult3988 {
    bool libOk = false;
    bool appOk = false;
    std::string err;
    std::string exePath;
};

XmodResult3988 compileImport3988(const std::string& libSrc,
                                 const std::string& appSrc) {
    auto dir = wtDir3988();
    std::string stem = unique_stem("bz3988_");
    XmodResult3988 out;
    TempFile3988 libSrcFile((dir / (stem + "_lib.bzl")).string());
    TempFile3988 libObjFile((dir / (stem + "_lib.o")).string());
    writeSource3988(libSrcFile.path, libSrc);
    {
        bezel::Driver driver;
        out.libOk = driver.compileObject(libSrcFile.path, libObjFile.path).success
            && std::ifstream(libObjFile.path).good();
    }
    if (!out.libOk) return out;
    TempFile3988 appSrcFile((dir / (stem + "_app.bzl")).string());
    std::string exePath = (dir / stem).string();
    writeSource3988(appSrcFile.path, appSrc);
    out.err = captureStderr3988([&] {
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

int runCapture3988(const std::string& exe) {
    int status = std::system(exe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

bool contains3988(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ── LIBRARY ──────────────────────────────────────────────────────────────
//
// fix #4001 interaction (§7.9/§9.4, v0.294): #4001 (landed in this same
// batch, AFTER this file) correctly extends §7.9 definite-assignment
// enforcement to a record's body field/auto-property — a combined-form
// record declaring a non-nullable body field with NEITHER a field
// initializer NOR an assigning explicit constructor is now REJECTED AT
// DECLARATION, on ordinary `new Combo3988(x)` construction grounds, wholly
// independent of `default(T)`/§17.11. `Combo3988`'s original shape (`public
// Extra: Widget3988;`, no initializer, no constructor) tripped exactly that
// new, correct check, breaking every test in this file at the LIBRARY
// compile stage (`r.libOk` false) regardless of which record the test
// actually exercises, since `kLib3988` is one shared source string. `Extra`
// now gets a field initializer sourced from a `static readonly` singleton
// (a borrow of an already-alive value, not "a freshly-produced owned value"
// — §9.7's record-field-borrow rule rejects the latter) so ordinary
// construction satisfies §7.9. This is orthogonal to what THIS file tests:
// §17.11's `default(T)` rule is a pure function of a field's DECLARED TYPE
// (`ExprTC::defaultIllFormed`, ExprTypecheck_Exprs.cpp) and does not consult
// whether the field has an initializer or an assigning constructor, so
// `default(Combo3988)` is verified below (still) to reject with the exact
// same §17.11 diagnostic as before this change.
const char* kLib3988 =
    "module Lib3988; "
    "public class Widget3988 "
    "  public Count: int; "
    "  public constructor(c: int) Count := c; end constructor; "
    "  public method Render(): int return Count; end Render; "
    "  public static readonly Shared3988: Widget3988 := new Widget3988(0); "
    "end Widget3988; "
    "public record Combo3988(X: int) "
    "  public Extra: Widget3988 := Widget3988.Shared3988; "
    "end Combo3988; "
    "public record Combo3988Nullable(X: int) "
    "  public Extra: Widget3988?; "
    "end Combo3988Nullable; "
    "public record BodyOnly3988 "
    "  public readonly property Extra: Widget3988; "
    "  public constructor(w: Widget3988) Extra := w; end constructor; "
    "end BodyOnly3988; "
    "public record InnerDel3988<T>(V: T); "
    "public record OuterDelCF3988<T>(X: int) "
    "  public In: InnerDel3988<T>; "
    "end OuterDelCF3988; "
    "public record OuterDelBO3988<T> "
    "  public In: InnerDel3988<T>; "
    "end OuterDelBO3988;";

// ── (1) the primary repro: `default(T)` over an imported COMBINED-FORM
//        record with a non-nullable class-typed BODY field must REJECT at
//        compile time, never reach codegen, never SIGSEGV ────────────────
BEZEL_TEST(xmod_default_record_combined_form_body_field_3988,
           combined_form_body_field_imported_record_rejected_spec_17_11) {
    const std::string app =
        "module App3988; "
        "import Lib3988; "
        "class Program3988 "
        "  public static method Main(): int "
        "    var x := default(Combo3988); "
        "    return x.Extra.Render(); "
        "  end Main; "
        "end Program3988;";
    auto r = compileImport3988(kLib3988, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(contains3988(
        r.err,
        "`default(T)` is a compile error for a record any of whose fields "
        "is a non-nullable reference type"));
    BEZEL_EXPECT_TRUE(contains3988(r.err, "(\xC2\xA7""17.11)"));
}

// ── (2) a body-ONLY imported record (not combined-form) with a non-nullable
//        class-typed auto-property must reject identically ────────────────
BEZEL_TEST(xmod_default_record_combined_form_body_field_3988,
           body_only_imported_record_rejected_spec_17_11) {
    const std::string app =
        "module App3988BodyOnly; "
        "import Lib3988; "
        "class Program3988BodyOnly "
        "  public static method Main(): int "
        "    var x := default(BodyOnly3988); "
        "    return x.Extra.Render(); "
        "  end Main; "
        "end Program3988BodyOnly;";
    auto r = compileImport3988(kLib3988, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(contains3988(
        r.err,
        "`default(T)` is a compile error for a record any of whose fields "
        "is a non-nullable reference type"));
    BEZEL_EXPECT_TRUE(contains3988(r.err, "(\xC2\xA7""17.11)"));
}

// ── (3, local twin, anti-regression) same combined-form shape declared
//        LOCALLY — must keep rejecting with a REAL compile-time location
//        (not a bare `!ran`, which a runtime crash also satisfies) ────────
BEZEL_TEST(xmod_default_record_combined_form_body_field_3988,
           combined_form_local_twin_still_rejects_spec_17_11) {
    const char* prog =
        "module AppLocal3988; "
        "class Widget3988Local "
        "  public Count: int; "
        "  public constructor(c: int) Count := c; end constructor; "
        "  public method Render(): int return Count; end Render; "
        "end Widget3988Local; "
        "record Combo3988Local(X: int) "
        "  public Extra: Widget3988Local; "
        "end Combo3988Local; "
        "class Program3988Local "
        "  public static method Main(): int "
        "    var x := default(Combo3988Local); "
        "    return x.Extra.Render(); "
        "  end Main; "
        "end Program3988Local;";
    auto r = bezel::test::runAot(prog);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    bool saw = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            contains3988(d.message,
                        "`default(T)` is a compile error for a record any "
                        "of whose fields is a non-nullable reference type"))
            saw = true;
    }
    BEZEL_EXPECT_TRUE(saw);
}

// ── (4, positive control) a NULLABLE body field on an imported combined-form
//        record is a valid null source — must keep compiling AND running,
//        with the value actually observed at runtime ──────────────────────
BEZEL_TEST(xmod_default_record_combined_form_body_field_3988,
           nullable_body_field_imported_record_positive_control) {
    const std::string app =
        "module App3988Pos; "
        "import Lib3988; "
        "class Program3988Pos "
        "  public static method Main(): int "
        "    var x := default(Combo3988Nullable); "
        "    if x.Extra <> null then return 1; end if; "
        "    return 7; "
        "  end Main; "
        "end Program3988Pos;";
    auto r = compileImport3988(kLib3988, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_TRUE(r.appOk);
    if (!r.appOk) return;
    BEZEL_EXPECT_EQ(runCapture3988(r.exePath), 7);
}

// ── (5) DELEGATING generic record through an IMPORTED COMBINED-FORM body
//        field (§3.6 rule 2 / §17.11 / #3995 composition), the intersection
//        of every axis this fix family touches: import (#3982) + generic
//        substitution (#3986) + composition across a delegating field
//        (#3995) + body/combined-form storage visibility (#3988). `In`'s
//        declared type `InnerDel3988<T>` is not itself a bare parameter, so
//        it needs the CURRENT frame's composed env threaded into the
//        recursive `defaultIllFormed` call for Inner's own bare `T` to
//        resolve back to `Widget3988`. MEASURED on this fix's immediate
//        parent (the #3986/#3982/#3995-merged arm, positional-fields-only):
//        compiled clean, exit 0, then SIGSEGV'd (exit 139) — the body field
//        `In` was never visible to the walk at all. ─────────────────────
BEZEL_TEST(xmod_default_record_combined_form_body_field_3988,
           delegating_generic_combined_form_imported_rejected_spec_17_11_3995) {
    const std::string app =
        "module App3988DelCF; "
        "import Lib3988; "
        "class Program3988DelCF "
        "  public static method Main(): int "
        "    var x := default(OuterDelCF3988<Widget3988>); "
        "    return x.In.V.Render(); "
        "  end Main; "
        "end Program3988DelCF;";
    auto r = compileImport3988(kLib3988, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(contains3988(
        r.err,
        "`default(T)` is a compile error for a record any of whose fields "
        "is a non-nullable reference type"));
    BEZEL_EXPECT_TRUE(contains3988(r.err, "(\xC2\xA7""17.11)"));
}

// ── (6) same delegating shape through IMPORTED BODY-ONLY storage (a plain
//        field, avoiding the unrelated #2884/#3994 accessor limitations a
//        property would confound this reject-path measurement with) ──────
BEZEL_TEST(xmod_default_record_combined_form_body_field_3988,
           delegating_generic_body_only_imported_rejected_spec_17_11_3995) {
    const std::string app =
        "module App3988DelBO; "
        "import Lib3988; "
        "class Program3988DelBO "
        "  public static method Main(): int "
        "    var x := default(OuterDelBO3988<Widget3988>); "
        "    return x.In.V.Render(); "
        "  end Main; "
        "end Program3988DelBO;";
    auto r = compileImport3988(kLib3988, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(contains3988(
        r.err,
        "`default(T)` is a compile error for a record any of whose fields "
        "is a non-nullable reference type"));
    BEZEL_EXPECT_TRUE(contains3988(r.err, "(\xC2\xA7""17.11)"));
}

// ── (7, positive control) the SAME imported delegating combined-form
//        shape, substituted at a VALUE type — must stay accepted with the
//        correct value ─────────────────────────────────────────────────
BEZEL_TEST(xmod_default_record_combined_form_body_field_3988,
           delegating_generic_combined_form_imported_value_leaf_positive_control) {
    const std::string app =
        "module App3988DelCFV; "
        "import Lib3988; "
        "class Program3988DelCFV "
        "  public static method Main(): int "
        "    var x := default(OuterDelCF3988<int>); "
        "    return x.X + x.In.V; "
        "  end Main; "
        "end Program3988DelCFV;";
    auto r = compileImport3988(kLib3988, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_TRUE(r.appOk);
    if (!r.appOk) return;
    BEZEL_EXPECT_EQ(runCapture3988(r.exePath), 0);
}

// ── (8, positive control) the SAME imported delegating body-only shape,
//        substituted at a NULLABLE class leaf — must stay accepted (null) ─
BEZEL_TEST(xmod_default_record_combined_form_body_field_3988,
           delegating_generic_body_only_imported_nullable_leaf_positive_control) {
    const std::string app =
        "module App3988DelBON; "
        "import Lib3988; "
        "class Program3988DelBON "
        "  public static method Main(): int "
        "    var x := default(OuterDelBO3988<Widget3988?>); "
        "    if x.In.V <> null then return 1; end if; "
        "    return 9; "
        "  end Main; "
        "end Program3988DelBON;";
    auto r = compileImport3988(kLib3988, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_TRUE(r.appOk);
    if (!r.appOk) return;
    BEZEL_EXPECT_EQ(runCapture3988(r.exePath), 9);
}

}  // namespace bezel::module
