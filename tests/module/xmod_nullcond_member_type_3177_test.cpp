// Spec-derived regression test for issue #3177 — family M4 ("member TYPE
// resolution at a use site") of the imported-symbol classification seam
// (umbrella #3152, member half #3149).
//
// §7.5 (null-conditional member access) / §9.1 (positional records) /
// §9.2 (body records) / §2.5 + §2.8.3 rule 4 (imported artifacts) /
// §14.1 (generic substitution), v0.254.
// Audit: docs/proposals/papers/member-symbol-audit-2026-08-13.md, family M4 and
// drift-pair #13 of §6.
//
// ── WHAT WAS MEASURED, on ec6a02f, with real two-module builds ─────────────
//
// `ExprTC::resolveNullCondMember` typed the accessed member by casting
// `Symbol::decl` to `ast::FieldDecl` / `ast::PropertyDecl` and, failing that,
// by reading `FieldSymbol::posParam->type`. `ModuleLoader` assigns neither for
// an imported member (and #2516's synthetic `posParam` carries a NULL `type`),
// so for every import the cast answered "no" and the `?.` node was left with no
// `TypeRef` at all. `lowerCoalesce` reads "no type" as "nothing" — so the
// expression silently produced a junk value:
//
//   shape (imported vs character-identical single-module twin)  imported  local
//   ----------------------------------------------------------  --------  -----
//   (q?.X  ?? 99)   nullable record, §9.1 positional field              1     11
//   (qn?.X ?? 99)   the same, receiver IS null                          0     99
//   (b?.F  ?? 99)   class body field                                    1     42
//   (b?.P  ?? 99)   class auto-property   (#3177's own repro)           1      7
//
// The second row is the sharpest: a NULL receiver did not even yield the `??`
// default. The value `1` is the null-check flag leaking out as the expression's
// value — so this is neither "the receiver was seen as null" nor "the member
// read zero". Every one of these compiled clean, exit 0, no diagnostic at any
// severity. A test that asserted only "compiles and exits 0" passed throughout,
// which is why every assertion below pins the VALUE.
//
// ── HOW IT IS FIXED ────────────────────────────────────────────────────────
//
// The member's declared type now comes from `sema::storageComponentType` — the
// one both-sources accessor (positional `Param` type, then body
// `FieldDecl`/`PropertyDecl` type, then the loader's `importedMemberType` side
// table) — followed by the same `substituteMemberType` call the `.` spelling
// has always made. `resolveMemberAccess` has had an imported arm since #503;
// `resolveNullCondMember` had none, which is audit drift-pair #13.
//
// ── THE WIDENING GUARD (T3/T4) ─────────────────────────────────────────────
//
// Typing the node is not free: `g.P` on an IMPORTED GENERIC type is refused by
// #2884 precisely because it has no working lower path (an `undefined
// reference` to a per-instantiation accessor). `g?.P` is a distinct AST node
// and consulted neither #2884's nor #2943's decline, so before this fix it
// compiled clean and printed `1`. Typing it without the decline would have
// converted that junk value into a LINK ERROR. T3 pins that the two spellings
// now share one decline. T4 pins the other direction: a LOCAL generic's
// `T`-typed member read through `?.` is now substituted against the receiver's
// concrete arguments, which turns a previously-FALSE-REJECT ("member `ToString`
// on `T` … not yet implemented … #2457") into the correct value.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

// Per-process unique working dir under the shared scratch base. The previous
// fs::temp_directory_path() (== /tmp tmpfs) was race-prone even with
// unique_stem: parallel shards racing on the SAME /tmp parent dir overwrote
// each other's _a.bzl / _a.o / _b.bzl mid-compileObject. The
// /var/tmp/bezel-scratch parent fixes that class of flake (#3002-class
// COVERAGE SHORT). popen() at L98 is kept — per-stem uniqueness already
// mitigates the file-overwrite race; full fork isolation is a separate concern.
const std::filesystem::path& wtDir3177() {
    return wtScratchDir("wt-3177");
}

struct TempFile3177 {
    std::string path;
    explicit TempFile3177(std::string p) : path(std::move(p)) {}
    ~TempFile3177() { std::remove(path.c_str()); }
};

void writeSource3177(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj3177(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

std::string runCapture3177(const std::string& exe, int& code) {
    std::string runCmd = exe + " 2>/dev/null";
    FILE* p = popen(runCmd.c_str(), "r");
    std::string out;
    if (!p) { code = -2; return out; }
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
        out.append(buf, n);
    int status = pclose(p);
    if (status == -1)          code = -2;
    else if (WIFSIGNALED(status)) code = -3;   // a signal is never a pass
    else if (!WIFEXITED(status))  code = -1;
    else                          code = WEXITSTATUS(status);
    return out;
}

// {exitCode, stdout}. -2 = the toolchain/dependency was unavailable (SKIP, and
// the ANTI-VACUITY guard: a malformed lib source can never make an expectation
// pass for the wrong reason); -1 = the importer failed to compile or link;
// -3 = the program died on a signal.
std::pair<int, std::string> compileImportRunCapture3177(
        const std::string& libSrc, const std::string& appSrc) {
    auto dir = wtDir3177();
    std::string stem = unique_stem("bz3177_");
    TempFile3177 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3177 aObj((dir / (stem + "_a.o")).string());
    writeSource3177(aSrc.path, libSrc);
    if (!compileToObj3177(aSrc.path, aObj.path)) return {-2, ""};
    if (!std::ifstream(aObj.path).good()) return {-2, ""};

    TempFile3177 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3177 bExe((dir / stem).string());
    writeSource3177(bSrc.path, appSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
             .success)
        return {-1, ""};

    int code = 0;
    std::string out = runCapture3177(bExe.path, code);
    return {code, out};
}

// The single-module control: the same program with no module boundary.
std::pair<int, std::string> compileLocalRunCapture3177(const std::string& src) {
    auto dir = wtDir3177();
    std::string stem = unique_stem("bz3177_local_");
    TempFile3177 sFile((dir / (stem + ".bzl")).string());
    TempFile3177 eFile((dir / stem).string());
    writeSource3177(sFile.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, sFile.path, eFile.path, {})
             .success)
        return {-1, ""};
    int code = 0;
    std::string out = runCapture3177(eFile.path, code);
    return {code, out};
}

// Compile the importer with stderr captured — for the DECLINE test (T3).
struct ImportReject3177 {
    bool dependencyCompiled = false;
    bool appCompiled = false;
    std::string error;
};

ImportReject3177 compileImportCaptureErr3177(const std::string& libSrc,
                                             const std::string& appSrc) {
    auto dir = wtDir3177();
    std::string stem = unique_stem("bz3177r_");
    TempFile3177 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3177 aObj((dir / (stem + "_a.o")).string());
    writeSource3177(aSrc.path, libSrc);
    ImportReject3177 out;
    out.dependencyCompiled = compileToObj3177(aSrc.path, aObj.path) &&
                             std::ifstream(aObj.path).good();
    if (!out.dependencyCompiled) return out;

    TempFile3177 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3177 bExe((dir / stem).string());
    writeSource3177(bSrc.path, appSrc);
    std::string errPath =
        (dir / ("bz3177_err_" + unique_stem("") + ".txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        out.appCompiled =
            bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
                .success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) { ::close(fd); ::dup2(saved, STDERR_FILENO); }
    ::close(saved);
    std::ifstream in(errPath);
    out.error.assign((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return out;
}

// ─── shared sources ──────────────────────────────────────────────────────────

// The dependency: a §9.1 positional record and a class carrying a body field
// and a §9.2 auto-property. Character-identical to the local twin's copies
// apart from the module boundary and the `L` name suffix.
const char* kLib3177 =
    "module DepM4_3177; "
    "public record Pt3177(X: int, Y: int); "
    "public class Box3177 "
    "  public F: int := 42; "
    "  public property P: int := 7; "
    "  public constructor() end constructor; "
    "end Box3177;";

// The body of `Main`, shared VERBATIM by the imported and the local program
// (only `@REC@`/`@BOX@` differ), so the sole difference between T1 and T2 is
// the module boundary. Every observable carries a NAMED discriminator: an exit
// status is 8-bit truncated and could not carry four values, and a bare number
// could not distinguish "the member's value" from "the null-check flag".
const char* kBody3177 =
    "    var q: @REC@? := new @REC@(11, 22); "
    "    var qn: @REC@? := null; "
    "    using var b := new @BOX@(); "
    "    Console.WriteLine(\"pos=\" + (q?.X ?? 99).ToString() + "
    "                      \"|null=\" + (qn?.X ?? 99).ToString() + "
    "                      \"|field=\" + (b?.F ?? 99).ToString() + "
    "                      \"|auto=\" + (b?.P ?? 99).ToString()); ";

std::string subst3177(const std::string& in, const std::string& marker,
                      const std::string& with) {
    std::string out = in;
    for (std::string::size_type i = out.find(marker);
         i != std::string::npos; i = out.find(marker, i + with.size()))
        out.replace(i, marker.size(), with);
    return out;
}

std::string body3177(const std::string& rec, const std::string& box) {
    return subst3177(subst3177(kBody3177, "@REC@", rec), "@BOX@", box);
}

const char kExpected3177[] = "pos=11|null=99|field=42|auto=7\n";

}  // namespace

// ===========================================================================
// ★ T1 — FAILS WITHOUT THE FIX. All four §7.5 accesses on IMPORTED members.
// Pre-fix stdout was "pos=1|null=0|field=1|auto=1"; the program compiled clean
// and exited 0 either way, so the VALUE is the whole assertion.
// // per §7.5 / §9.1 / §9.2 / §2.5 / §2.8.3 rule 4, v0.254
// ===========================================================================
BEZEL_TEST(xmod_nullcond_member_type_3177,
           imported_member_through_null_conditional_has_declared_type_7_5) {
    std::string app =
        "module AppM4_3177; import DepM4_3177; "
        "public class C3177 "
        "  public static method Main(): int " +
        body3177("Pt3177", "Box3177") +
        "    return 0; "
        "  end Main; "
        "end C3177;";
    auto [code, out] = compileImportRunCapture3177(kLib3177, app);
    BEZEL_EXPECT_TRUE(code != -2);  // anti-vacuity: the dependency compiled
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_EQ(out, std::string(kExpected3177));
}

// ===========================================================================
// ★ T2 — THE DIFFERENTIAL CONTROL. The character-identical program with both
// types declared in the SAME module. It was already correct before the fix and
// must stay correct: it is what pins that the module boundary was the entire
// difference, and that routing through `storageComponentType` did not change
// the local answer. // per §7.5 / §9.1 / §9.2 / §2.8.3 rule 4
// ===========================================================================
BEZEL_TEST(xmod_nullcond_member_type_3177,
           local_twin_null_conditional_unchanged_control_7_5) {
    std::string src =
        "module LocalM4_3177; "
        "public record Pt3177L(X: int, Y: int); "
        "public class Box3177L "
        "  public F: int := 42; "
        "  public property P: int := 7; "
        "  public constructor() end constructor; "
        "end Box3177L; "
        "public class C3177L "
        "  public static method Main(): int " +
        body3177("Pt3177L", "Box3177L") +
        "    return 0; "
        "  end Main; "
        "end C3177L;";
    auto [code, out] = compileLocalRunCapture3177(src);
    BEZEL_EXPECT_TRUE(code != -2);
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_EQ(out, std::string(kExpected3177));
}

// ===========================================================================
// ★ T3 — THE WIDENING GUARD. `g?.P` on an IMPORTED GENERIC type must be
// refused by the SAME #2884 decline the `.` spelling issues. Before this fix
// the `?.` spelling reached NEITHER #2884's nor #2943's decline (they live in
// `resolveMemberAccess`, and a NullCondMemberExpr is a distinct AST node), so
// it compiled clean and printed `1`. Typing the node without sharing the
// decline would have turned that junk value into an `undefined reference` at
// link time. // per §14.1 / §2.8.3 rule 4 / #2884
// ===========================================================================
BEZEL_TEST(xmod_nullcond_member_type_3177,
           imported_generic_property_declined_through_null_conditional_14_1) {
    auto r = compileImportCaptureErr3177(
        "module DepGen3177; "
        "public class GBox3177<T> "
        "  public property V: T; "
        "  public constructor(v: T) V := v; end constructor; "
        "end GBox3177;",
        "module AppGen3177; import DepGen3177; "
        "public class CG3177 "
        "  public static method Main(): int "
        "    using var g := new GBox3177<int>(7); "
        "    Console.WriteLine((g?.V ?? 99).ToString()); "
        "    return 0; "
        "  end Main; "
        "end CG3177;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);  // anti-vacuity
    if (!r.dependencyCompiled) return;
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(r.error.find("is not yet monomorphized") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("#2884") != std::string::npos);
}

// ===========================================================================
// ★ T4 — WHAT THE WIDENING NOW ADMITS. A LOCAL generic class's `T`-typed
// member read through `?.` is now substituted against the receiver's concrete
// arguments, exactly as the `.` spelling has done since #361. Before the fix
// the `?.` node was typed as the bare parameter `T`, which made this program a
// FALSE REJECT ("member `ToString` on `T` … not yet implemented … #2457"), so
// this test fails without the fix by failing to COMPILE. // per §14.1 / §7.5
// ===========================================================================
BEZEL_TEST(xmod_nullcond_member_type_3177,
           local_generic_member_substituted_through_null_conditional_14_1) {
    auto [code, out] = compileLocalRunCapture3177(
        "module LocalGen3177; "
        "public class GBox3177L<T> "
        "  public V: T; "
        "  public constructor(v: T) V := v; end constructor; "
        "end GBox3177L; "
        "public class CGL3177 "
        "  public static method Main(): int "
        "    using var g := new GBox3177L<int>(7); "
        "    Console.WriteLine(\"gen=\" + (g?.V ?? 99).ToString()); "
        "    return 0; "
        "  end Main; "
        "end CGL3177;");
    BEZEL_EXPECT_TRUE(code != -2);
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_EQ(out, std::string("gen=7\n"));
}

}  // namespace bezel::module
