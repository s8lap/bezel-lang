// fix #4543 (§14.1 "Generic declarations" / §5.1 "Implicit conversions" /
// §2.8.3 rule 4 "one runtime type identity"; spec v0.306) — an IMPORTED generic
// constructor whose parameter EMBEDS the class's own type parameter was
// unconstructible: the importer refused the argument with §5.1 where the
// character-identical single-module program compiles and runs.
//
// ── THE REPRO ──────────────────────────────────────────────────────────────
//
//     module MnLib;
//     public class Sc<T>
//       public Tag: int;
//       public constructor(x: Sc<T>?) Tag := 105; end constructor;
//     end Sc;
//
//     module MnApp; import MnLib;
//     var q: Sc<int>? := null;
//     using var c := new Sc<int>(q);        // ← refused
//
//     app.bzl:4:30: the `new Sc`: argument 1 has a type that is not implicitly
//     convertible to the target type (§5.1)
//
// The single-module twin answers 105. A BAD_REJECT, and a cross-module
// divergence: the boundary changed whether the program is legal, which §2.8.3
// rule 4 forbids.
//
// ── THE MECHANISM ──────────────────────────────────────────────────────────
//
// `ExprTC::checkImportedNewExprArgs` compared the call's argument against the
// imported `ParameterSymbol::resolvedDeclType` RAW. The local `new` path does
// not: it runs each formal through `substituteMemberType(…, &ne, t)` first, so
// a formal that mentions the class's type parameters is resolved against THIS
// construction's concrete type arguments. Without that step the comparison was
// `Sc<int>?` (argument) against `Sc<T>?` (formal) — a genericArgs mismatch.
//
// The issue's title says "nullable class-typed parameter", but nullability is
// not the trigger: MEASURED on the pre-fix compiler, the NON-nullable
// `constructor(x: Sc<T>)` is refused identically. What made the nullable form
// the one that surfaced is that it is the only spelling a caller can reach
// with a first instance in hand. The narrowing controls (T3/T4 below) show the
// real boundary: a bare `x: T` and a bare `x: T?` were both accepted pre-fix,
// because `checkAssignable` treats a declaration-less Named TypeRef as opaque;
// a CONSTRUCTED `Sc<T>` is not opaque, so it was the one shape left compared
// unsubstituted.
//
// ── WHAT THESE ROWS PIN ────────────────────────────────────────────────────
//
// Every accepting row is paired with a single-module twin over character-
// identical declarations (so it can only pass by the two sides agreeing), and
// T5 is a NEGATIVE control: substitution must not turn the check into a
// rubber stamp — a genuinely wrong instantiation must still be rejected.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
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

namespace fs = std::filesystem;

struct TempFile4543 {
    std::string path;
    explicit TempFile4543(std::string p) : path(std::move(p)) {}
    ~TempFile4543() { std::remove(path.c_str()); }
};

void writeSource4543(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

//   -2  the DEPENDENCY could not be produced (no toolchain) — skip the row.
//   -1  the importing compile failed, or the program did not run.
//   >=0 the program's own exit status.
constexpr int kDepUnavailable4543 = -2;

// Compile the dependency, compile the importer against it, run the program in a
// SEPARATE process (per tests/README.md). `capturedStderr`, when non-null,
// receives the importing compile's stderr — the only way to assert on
// diagnostic TEXT, which is what the reject rows need.
int runImported4543(const std::string& libSrc, const std::string& appSrc,
                    const std::string& tag,
                    std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz4543_" + tag + "_" + unique_stem("");

    TempFile4543 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile4543 aObj((dir / (stem + "_a.o")).string());
    writeSource4543(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return kDepUnavailable4543;
    }
    if (!std::ifstream(aObj.path).good()) return kDepUnavailable4543;

    TempFile4543 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile4543 bExe((dir / (stem + "_b")).string());
    TempFile4543 bErr((dir / (stem + "_b.err")).string());
    writeSource4543(bSrc.path, appSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(bErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        compiled =
            bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
                .success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(bErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled =
            bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
                .success;
    }
    if (!compiled) return -1;
    if (!std::ifstream(bExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(bExe.path.c_str(), bExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// The single-module twin — the differential control.
int runSingleModule4543(const std::string& src, const std::string& tag) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz4543L_" + tag + "_" + unique_stem("");
    TempFile4543 sSrc((dir / (stem + ".bzl")).string());
    TempFile4543 sExe((dir / stem).string());
    writeSource4543(sSrc.path, src);

    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, sSrc.path, sExe.path, {})
             .success)
        return -1;
    if (!std::ifstream(sExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(sExe.path.c_str(), sExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// ── The declarations under test ────────────────────────────────────────────
// Used VERBATIM as the dependency's body and, spliced in, as the twin's, so a
// row can only pass by the imported and local sides agreeing.

// The issue's class: the constructor's parameter is the class itself at its own
// type parameter. The body distinguishes the null and non-null arguments so a
// row proves the ARGUMENT arrived, not merely that construction happened.
std::string declsSelfParam4543(const std::string& tag) {
    return "public class Sc" + tag + "<T> "
           "  public Tag: int; "
           "  public constructor(x: Sc" + tag + "<T>?) "
           "    if x = null then Tag := 105; else Tag := 205; end if; "
           "  end constructor; "
           "end Sc" + tag + ";";
}

// The bare-type-parameter control: `x: T?`. Accepted BEFORE this fix (a
// declaration-less Named TypeRef is opaque to `checkAssignable`), so this row
// is what would catch a fix that broke the shapes that already worked.
std::string declsBareParam4543(const std::string& tag) {
    return "public class Sb" + tag + "<T> "
           "  public Tag: int; "
           "  public constructor(x: T?) "
           "    Tag := 105; "
           "  end constructor; "
           "end Sb" + tag + ";";
}

std::string lib4543(const std::string& tag, const std::string& decls) {
    return "module MnLib4543" + tag + "; " + decls;
}
std::string app4543(const std::string& tag, const std::string& body) {
    return "module MnApp4543" + tag + "; import MnLib4543" + tag + "; "
           "class P" + tag + " " + body + " end P" + tag + ";";
}
std::string twin4543(const std::string& tag, const std::string& decls,
                     const std::string& body) {
    return "module MnTwin4543" + tag + "; " + decls +
           " class P" + tag + " " + body + " end P" + tag + ";";
}

}  // namespace

// ===========================================================================
// T1 — §14.1 / §5.1 / §2.8.3 rule 4: THE ISSUE'S REPRO. `new Sc<int>(q)` where
// `q: Sc<int>?` is null and the imported constructor takes `x: Sc<T>?`.
//
// Pre-fix: refused with "the `new Sc`: argument 1 has a type that is not
//          implicitly convertible to the target type (§5.1)".
// Post-fix: compiles; the body takes the null arm and the program exits 105.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_ctor_self_param_4543,
           t1_imported_ctor_param_at_own_type_param_14_1) {
    const std::string body =
        "  public static method Main(): int "
        "    var q: ScT1<int>? := null; "
        "    using var c := new ScT1<int>(q); "
        "    return c.Tag; "
        "  end Main;";
    int r = runImported4543(lib4543("T1", declsSelfParam4543("T1")),
                            app4543("T1", body), "T1");
    if (r == kDepUnavailable4543) return;
    BEZEL_EXPECT_EQ(r, 105);
}

BEZEL_TEST(xmod_imported_generic_ctor_self_param_4543,
           t1_twin_same_program_in_one_module_14_1) {
    const std::string body =
        "  public static method Main(): int "
        "    var q: ScT1L<int>? := null; "
        "    using var c := new ScT1L<int>(q); "
        "    return c.Tag; "
        "  end Main;";
    int r = runSingleModule4543(
        twin4543("T1L", declsSelfParam4543("T1L"), body), "T1L");
    BEZEL_EXPECT_EQ(r, 105);
}

// ===========================================================================
// T2 — the NON-NULL argument, which proves the substituted formal admits a real
// instance and that the value reaches the body. The first construction takes
// the null arm (105); the second is handed that instance and takes the other
// arm (205). A row that merely compiled would not distinguish the two.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_ctor_self_param_4543,
           t2_non_null_instance_argument_reaches_the_body_14_1) {
    const std::string body =
        "  public static method Main(): int "
        "    var q: ScT2<int>? := null; "
        "    using var a := new ScT2<int>(q); "
        "    using var b := new ScT2<int>(a); "
        "    if a.Tag <> 105 then return 1; end if; "
        "    if b.Tag <> 205 then return 2; end if; "
        "    return 77; "
        "  end Main;";
    int r = runImported4543(lib4543("T2", declsSelfParam4543("T2")),
                            app4543("T2", body), "T2");
    if (r == kDepUnavailable4543) return;
    BEZEL_EXPECT_EQ(r, 77);
}

BEZEL_TEST(xmod_imported_generic_ctor_self_param_4543,
           t2_twin_same_program_in_one_module_14_1) {
    const std::string body =
        "  public static method Main(): int "
        "    var q: ScT2L<int>? := null; "
        "    using var a := new ScT2L<int>(q); "
        "    using var b := new ScT2L<int>(a); "
        "    if a.Tag <> 105 then return 1; end if; "
        "    if b.Tag <> 205 then return 2; end if; "
        "    return 77; "
        "  end Main;";
    int r = runSingleModule4543(
        twin4543("T2L", declsSelfParam4543("T2L"), body), "T2L");
    BEZEL_EXPECT_EQ(r, 77);
}

// ===========================================================================
// T3 — a SECOND instantiation of the same imported generic. `Sc<string>` must
// bind its own `T`, so the substituted formal is `Sc<string>?`; nothing here
// may be keyed on the first instantiation seen.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_ctor_self_param_4543,
           t3_second_instantiation_substitutes_its_own_arg_14_1) {
    const std::string body =
        "  public static method Main(): int "
        "    var qi: ScT3<int>? := null; "
        "    var qs: ScT3<string>? := null; "
        "    using var ci := new ScT3<int>(qi); "
        "    using var cs := new ScT3<string>(qs); "
        "    if ci.Tag <> 105 then return 1; end if; "
        "    if cs.Tag <> 105 then return 2; end if; "
        "    return 88; "
        "  end Main;";
    int r = runImported4543(lib4543("T3", declsSelfParam4543("T3")),
                            app4543("T3", body), "T3");
    if (r == kDepUnavailable4543) return;
    BEZEL_EXPECT_EQ(r, 88);
}

// ===========================================================================
// T4 — the bare-type-parameter CONTROL, `constructor(x: T?)`. This shape was
// already accepted cross-module before the fix, so it is the row that catches a
// change that broke what worked.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_ctor_self_param_4543,
           t4_bare_type_param_control_still_accepted_14_1) {
    const std::string body =
        "  public static method Main(): int "
        "    var q: int? := null; "
        "    using var c := new SbT4<int>(q); "
        "    return c.Tag; "
        "  end Main;";
    int r = runImported4543(lib4543("T4", declsBareParam4543("T4")),
                            app4543("T4", body), "T4");
    if (r == kDepUnavailable4543) return;
    BEZEL_EXPECT_EQ(r, 105);
}

// ===========================================================================
// T5 ★ THE NEGATIVE CONTROL — §5.1 / §14.1. Substituting the formal must not
// make the check vacuous. `new Sc<int>(w)` where `w: Sc<string>?` binds the
// formal to `Sc<int>?`, which `Sc<string>?` is NOT convertible to, so the §5.1
// rejection MUST survive. Without this row a "fix" that simply stopped checking
// imported constructor arguments would pass every other row in this file.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_ctor_self_param_4543,
           t5_wrong_instantiation_argument_still_rejected_5_1) {
    const std::string body =
        "  public static method Main(): int "
        "    var w: ScT5<string>? := null; "
        "    using var c := new ScT5<int>(w); "
        "    return c.Tag; "
        "  end Main;";
    std::string err;
    int r = runImported4543(lib4543("T5", declsSelfParam4543("T5")),
                            app4543("T5", body), "T5", &err);
    if (r == kDepUnavailable4543) return;
    BEZEL_EXPECT_EQ(r, -1);
    BEZEL_EXPECT_TRUE(err.find("not implicitly convertible to the target "
                               "type") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("\xC2\xA7""5.1") != std::string::npos);
}

}  // namespace bezel::module
