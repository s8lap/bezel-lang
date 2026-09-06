// Three-way merge-verification test for #3982 + #3986 + #3988 (§17.11 /
// §9.3, v0.294): all three fixes rewrite the SAME `ExprTC::defaultIllFormed`
// `Record` arm (ExprTypecheck_Exprs.cpp), each closing a different axis:
//
//   - #3982 closed the IMPORT axis: the arm cast `t->declaration->decl` to
//     `ast::TypeDecl*` and accepted `default(T)` whenever that cast failed
//     (always, for an imported type). Fixed by reading the SYMBOL side
//     (`recordPositionalFieldSymbols` + `storageComponentType`).
//
//   - #3986 closed the SUBSTITUTION axis: a generic record's bare-type-
//     parameter field resolves to an unsubstituted, declaration-less
//     TypeRef, which the function's own `!t->declaration` early return
//     unconditionally accepts regardless of the concrete instantiation.
//     Fixed by threading a `common::RecordSubstEnv` through the walk.
//
//   - #3988 closed the STORAGE-SHAPE axis: the walk covered only
//     POSITIONAL fields (`recordPositionalFieldSymbols`), so a combined-
//     form or body-only record's BODY field/auto-property was invisible
//     regardless of the other two axes. Fixed by switching the walk to
//     `recordStorageMembers` (positional + body + auto-properties), which
//     already accepts the SAME `env` #3986 seeds and applies
//     `common::resolveRecordFieldSubst` to each member internally
//     (SymbolCollection.cpp, fix #3171) — so the merged arm gets full
//     enumeration AND substitution from one call.
//
// THE INTERSECTION CELL none of the three fixes' own suites covers alone:
// an IMPORTED, COMBINED-FORM (or body-only) GENERIC record whose BODY
// field's type is a bare type parameter bound to a non-nullable class. This
// is the one cell where a plausible-but-wrong three-way merge could pass
// every one of the three fixes' own suites while still silently accepting a
// crashing program — e.g. a merge that fed `recordStorageMembers` a null
// `env` (losing #3986's axis while keeping #3982's and #3988's), or one
// that kept `recordPositionalFieldSymbols` for the walk and bolted
// substitution onto it (losing #3988's axis while keeping #3982's and
// #3986's), or one that reintroduced the AST-side `seedRecordSubstEnv`
// overload (losing #3982's axis for every imported generic record).
//
// Two library records exercise both storage shapes named in the coordinator
// brief:
//   - `BoxCFM(T)(X: int) { public V: T; }`      — COMBINED FORM (a
//     positional field `X` alongside a body field `V: T`, §9.3)
//   - `BoxBOM<T> { public readonly property V: T; constructor(v: T) ... }`
//                                                — BODY-ONLY, an auto-property
//
// Both are declared in an imported module and instantiated with a
// non-nullable class type argument for `T` at `default(...)`.
//
// MEASURED on the three-way-merged arm (this fix's own commit): both
// combination cells REJECT at compile time with the same §17.11 message
// text every other control in this family produces, and never reach
// codegen. Anti-regression controls substitute the SAME two imported
// generic records at a value type and at a nullable class type and confirm
// both stay accepted with the correct runtime value.

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

const std::filesystem::path& wtDir3W() { return wtScratchDir("wt-3w"); }

struct TempFile3W {
    std::string path;
    explicit TempFile3W(std::string p) : path(std::move(p)) {}
    ~TempFile3W() { std::remove(path.c_str()); }
};

void writeSource3W(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

std::string captureStderr3W(const std::function<void()>& body) {
    std::string errPath =
        (wtDir3W() / ("err_" + unique_stem("") + ".txt")).string();
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

struct XmodResult3W {
    bool libOk = false;
    bool appOk = false;
    std::string err;
    std::string exePath;
};

XmodResult3W compileImport3W(const std::string& libSrc,
                             const std::string& appSrc) {
    auto dir = wtDir3W();
    std::string stem = unique_stem("bz3w_");
    XmodResult3W out;
    TempFile3W libSrcFile((dir / (stem + "_lib.bzl")).string());
    TempFile3W libObjFile((dir / (stem + "_lib.o")).string());
    writeSource3W(libSrcFile.path, libSrc);
    {
        bezel::Driver driver;
        out.libOk = driver.compileObject(libSrcFile.path, libObjFile.path).success
            && std::ifstream(libObjFile.path).good();
    }
    if (!out.libOk) return out;
    TempFile3W appSrcFile((dir / (stem + "_app.bzl")).string());
    std::string exePath = (dir / stem).string();
    writeSource3W(appSrcFile.path, appSrc);
    out.err = captureStderr3W([&] {
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

int runCapture3W(const std::string& exe) {
    int status = std::system(exe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

bool contains3W(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ── LIBRARY: a COMBINED-FORM generic record (positional field + a bare-
// type-param BODY field) and a BODY-ONLY generic record (a bare-type-param
// auto-property), both imported. ───────────────────────────────────────────

const char* kLib3W =
    "module Lib3W; "
    "public class Widget3W "
    "  public Count: int; "
    "  public constructor(c: int) Count := c; end constructor; "
    "  public method Render(): int return Count; end Render; "
    "end Widget3W; "
    "public record BoxCF3W<T>(X: int) "
    "  public V: T; "
    "end BoxCF3W; "
    "public record BoxBO3W<T> "
    "  public readonly property V: T; "
    "  public constructor(v: T) V := v; end constructor; "
    "end BoxBO3W;";

// ── THE INTERSECTION CELL, combined form: imported + generic + combined-
// form BODY field + non-nullable substitution. Must REJECT. ───────────────

BEZEL_TEST(xmod_generic_combined_form_body_field_default_subst_three_way_merge,
           combined_form_body_field_nonnullable_class_param_rejected) {
    const std::string app =
        "module App3WCF; "
        "import Lib3W; "
        "class Program3WCF "
        "  public static method Main(): int "
        "    var x := default(BoxCF3W<Widget3W>); "
        "    return x.V.Render(); "
        "  end Main; "
        "end Program3WCF;";
    auto r = compileImport3W(kLib3W, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(contains3W(
        r.err,
        "`default(T)` is a compile error for a record any of whose fields "
        "is a non-nullable reference type"));
    BEZEL_EXPECT_TRUE(contains3W(r.err, "(\xC2\xA7""17.11)"));
}

// ── THE INTERSECTION CELL, body-only: imported + generic + body-only
// auto-property + non-nullable substitution. Must REJECT. ─────────────────

BEZEL_TEST(xmod_generic_combined_form_body_field_default_subst_three_way_merge,
           body_only_auto_property_nonnullable_class_param_rejected) {
    const std::string app =
        "module App3WBO; "
        "import Lib3W; "
        "class Program3WBO "
        "  public static method Main(): int "
        "    var x := default(BoxBO3W<Widget3W>); "
        "    return x.V.Render(); "
        "  end Main; "
        "end Program3WBO;";
    auto r = compileImport3W(kLib3W, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(contains3W(
        r.err,
        "`default(T)` is a compile error for a record any of whose fields "
        "is a non-nullable reference type"));
    BEZEL_EXPECT_TRUE(contains3W(r.err, "(\xC2\xA7""17.11)"));
}

// ── Anti-regression: the SAME imported combined-form generic record,
// substituted at a VALUE type, must stay defaultable. ─────────────────────

BEZEL_TEST(xmod_generic_combined_form_body_field_default_subst_three_way_merge,
           combined_form_body_field_value_param_still_accepted) {
    const std::string app =
        "module App3WCFV; "
        "import Lib3W; "
        "class Program3WCFV "
        "  public static method Main(): int "
        "    var x := default(BoxCF3W<int>); "
        "    return x.X + x.V; "
        "  end Main; "
        "end Program3WCFV;";
    auto r = compileImport3W(kLib3W, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_TRUE(r.appOk);
    if (!r.appOk) return;
    BEZEL_EXPECT_EQ(runCapture3W(r.exePath), 0);
}

// ── Anti-regression: the SAME imported combined-form generic record,
// substituted at a NULLABLE class type, must stay defaultable (null). ─────

BEZEL_TEST(xmod_generic_combined_form_body_field_default_subst_three_way_merge,
           combined_form_body_field_nullable_param_still_accepted) {
    const std::string app =
        "module App3WCFN; "
        "import Lib3W; "
        "class Program3WCFN "
        "  public static method Main(): int "
        "    var x := default(BoxCF3W<Widget3W?>); "
        "    if x.V <> null then return 1; end if; "
        "    return 9; "
        "  end Main; "
        "end Program3WCFN;";
    auto r = compileImport3W(kLib3W, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_TRUE(r.appOk);
    if (!r.appOk) return;
    BEZEL_EXPECT_EQ(runCapture3W(r.exePath), 9);
}

// ── Anti-regression: the SAME imported body-only generic record,
// substituted at a NULLABLE class type, must stay ACCEPTED (defaultable) —
// value observation is deliberately NOT asserted here. Reading `x.V`
// directly is rejected at compile time by the PRE-EXISTING, correctly-
// diagnosed #2884 limitation ("a generic type's accessors are not exported
// per instantiation" — unrelated to this fix); routing through a method
// instead (its own suggested remedy) was measured, while writing this test,
// to return the WRONG value for a reference-typed generic argument on an
// IMPORTED generic record — a genuine, distinct, orthogonal codegen defect
// (construction/accessor marshalling, not this arm's accept/reject verdict)
// filed as issue 3994 with the full isolating repro (both `default(...)`
// and `new ...(null)` affected; a LOCAL, same-module twin of this exact
// program is correct). Neither limitation is reachable through this fix's
// own combined-form sibling control above, which uses a plain FIELD (not an
// auto-property) and reads the correct value. So this control asserts only
// what THIS fix owns: `default(T)` over this shape must compile and run
// without crashing. ─────────────────────────────────────────────────────
BEZEL_TEST(xmod_generic_combined_form_body_field_default_subst_three_way_merge,
           body_only_auto_property_nullable_param_still_accepted) {
    const std::string app =
        "module App3WBON; "
        "import Lib3W; "
        "class Program3WBON "
        "  public static method Main(): int "
        "    var x := default(BoxBO3W<Widget3W?>); "
        "    return 9; "
        "  end Main; "
        "end Program3WBON;";
    auto r = compileImport3W(kLib3W, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_TRUE(r.appOk);
    if (!r.appOk) return;
    BEZEL_EXPECT_EQ(runCapture3W(r.exePath), 9);
}

}  // namespace bezel::module
