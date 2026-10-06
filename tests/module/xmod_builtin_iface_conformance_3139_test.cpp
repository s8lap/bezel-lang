// Spec-derived regression test for issue #3139 — family F1 of the
// imported-symbol classification seam (umbrella #3152).
//
// spec §18.5 (format specs) / §14.2 (generic constraints) / §2.5 (imported
// declarations) / §2.8.2–§2.8.3 (module artifacts), v0.254. Audit:
// docs/proposals/imported-symbol-audit-2026-08-13.md, family F1.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// A built-in interface — `ISerializable`, `IDeserializable`, `IFormattable`,
// `IEnumerable<T>`, the §4 collection family — has no user `TypeSymbol`. Type
// Resolution's base-type loop pushes a base onto `TypeSymbol::baseTypes` /
// `baseTypeRefs` only `if (ref->declaration)`, so such a base was dropped from
// both lists on the LOCAL side too, and `MetaTypeDecl::baseTypes` is built from
// `baseTypeRefs`, so it never reached the wire either.
//
// Every consumer therefore recovered the conformance by walking
// `ast::TypeDecl::baseTypes`:
//
//     dynamic_cast<const ast::TypeDecl*>(ts->decl)   // null for EVERY import
//
// `ModuleLoader` never assigns `Symbol::decl` for an imported type — there is
// no `ast::TypeDecl` to assign, because the importing compilation has no AST
// for a dependency's types at all. So every one of those guards took its
// `false` branch, and "declares no built-in interface" is a substantive claim
// that is simply wrong across a module boundary. Unlike most of the seam this
// family fails LOUDLY: a positive-list-missing shape, i.e. a false REJECT of a
// program whose two halves each compile.
//
// The fix carries the fact on the symbol — `TypeSymbol::nonSymbolBaseRefs`, a
// `MetaTypeDecl::builtinInterfaceBases` record, and one accessor pair
// (`sema::declaresBuiltinInterface` / `inheritsBuiltinInterface`) whose walk
// never requires an `ast::TypeDecl` at any level. That is the #2950
// (`builtinExceptionBaseOf`) template applied to a second fact.
//
// ── MEASURED, with real two-module builds ──────────────────────────────────
//   (`bezelc -c lib.bzl -o lib.o` then `bezelc --import lib.o app.bzl`)
//
//                                             before (main @ 701cb2d)   after
//   §18.5 spec on imported `: IFormattable`    REJECT "`Money` is        42
//                                              neither (§18.5)"
//   §14.2 `Array<T>.Sort()`, imported          REJECT "requires T to     27
//         `: IComparable<T>` element           implement IComparable<T>
//                                              (§3 / #472)"
//   the SAME two programs written LOCALLY      42 / 27                   42 / 27
//
// The local twins are the point of the file: the programs are otherwise
// identical, so what differed was not the shape but the MODULE BOUNDARY. They
// are also the differential control — they passed before the fix and must keep
// passing, which is what pins that the change did not alter the local answer.
//
// ── THE NEGATIVE ARM ───────────────────────────────────────────────────────
//
// One F1 site pulled the OTHER way. The §14.2 `Min()`/`Max()` gate initialised
// its verdict to `true` ("string always satisfies IComparable") and only set it
// to `false` inside `if (td)` — so for an imported element the AST-null guard
// produced an unconditional ACCEPT of a NON-conforming element, which is the
// codegen IR-verifier failure that gate exists to prevent (the site's own
// comment: "sema types the result as T and codegen IR-fails"). So a
// non-conforming imported element must still be REJECTED after the fix, and
// `reject_non_comparable_imported_element` pins that direction.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // the ONE sanitizer-honouring compileFile wrapper
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile3139 {
    std::string path;
    explicit TempFile3139(std::string p) : path(std::move(p)) {}
    ~TempFile3139() { std::remove(path.c_str()); }
};

void writeSource3139(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The three failure modes are DISTINCT sentinels, because this family's defect
// IS a false compile reject and the reject arms assert on it directly. Folding
// "did not compile" together with "did not exit normally" would let a SIGSEGV or
// a dependency failure keep a reject arm green — the arm would be asserting
// "something went wrong" rather than "sema rejected it".
//
// ⚠ ONE GAP REMAINS, and it is not closable here. An importer LINK failure still
// maps to kCompileRejected: `Driver::compileFile` returns
// `CompileResult{false, cc.errorCount + 1}` for a link error exactly as it does
// for a sema reject (src/driver/Driver.cpp), so the two are indistinguishable to
// a caller. Constructed and measured:
// `env PATH=/nonexistent bezelc -o out --import lib.o app.bzl` on a sema-clean
// program prints "bezelc: link error" and exits 1. Closing it needs a
// Driver-level discriminator, not a test-side one. The lit track pins the
// diagnostic TEXT for these arms, which is the real protection.
//
// (The reject arms additionally pin the DIAGNOSTIC TEXT, which an exit code
// cannot carry at all, in the lit/FileCheck track:
// tests/lit/xmod_builtin_iface_conformance_reject_3139.bzl and
// tests/lit/builtin_iface_shadow_reject_3139.bzl. That is what distinguishes
// the §14.2 reject arriving at the #1198 gate — where this fix moves it — from
// the same program failing at the downstream #2770 backstop where it used to.)
constexpr int kDepFailed3139       = -2;  // the exporter itself failed
constexpr int kCompileRejected3139 = -1;  // the IMPORTER did not compile
constexpr int kRanAbnormally3139   = -3;  // built, but did not exit normally

int compileImportAndRun3139(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3139_");
    TempFile3139 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile3139 lObj((dir / (stem + "_lib.o")).string());
    writeSource3139(lSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(lSrc.path, lObj.path).success)
            return kDepFailed3139;
    }
    if (!std::ifstream(lObj.path).good()) return kDepFailed3139;

    TempFile3139 aSrc((dir / (stem + "_app.bzl")).string());
    TempFile3139 aExe((dir / stem).string());
    writeSource3139(aSrc.path, appSrc);
    bezel::Driver driver;
    bezel::Dependency d;
    d.metadataPath = lObj.path;
    d.objectPath = lObj.path;
    std::vector<bezel::Dependency> deps{d};
    if (!bezel::test::compileFileForTest(driver, aSrc.path, aExe.path, deps)
             .success)
        return kCompileRejected3139;
    int status = std::system(aExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status)
                                               : kRanAbnormally3139;
}

// Captures stderr from the importer compile.  Returns a pair: the exit status
// (kCompileRejected3139 if the importer rejected, kDepFailed3139 if the
// exporter failed, kRanAbnormally3139 if it ran abnormally) and the captured
// stderr text (empty if the compile succeeded or the exporter failed).
// This allows a reject row to assert the SPECIFIC diagnostic text, not just
// that the compilation failed.
std::pair<int, std::string> compileImportAndGetDiag3139(
    const std::string& libSrc,
    const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3139_");
    TempFile3139 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile3139 lObj((dir / (stem + "_lib.o")).string());
    writeSource3139(lSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(lSrc.path, lObj.path).success)
            return {kDepFailed3139, {}};
    }
    if (!std::ifstream(lObj.path).good()) return {kDepFailed3139, {}};

    TempFile3139 aSrc((dir / (stem + "_app.bzl")).string());
    TempFile3139 aExe((dir / stem).string());
    writeSource3139(aSrc.path, appSrc);

    // Capture stderr from the importer compile.  The driver prints diagnostics
    // to stderr, so we redirect it to a pipe and read the diagnostic text.
    int pipefd[2];
    if (pipe(pipefd) < 0) return {kRanAbnormally3139, {}};
    std::fflush(nullptr);
    int savedStderr = dup(STDERR_FILENO);
    if (savedStderr < 0) { close(pipefd[0]); close(pipefd[1]); return {kRanAbnormally3139, {}}; }
    if (dup2(pipefd[1], STDERR_FILENO) < 0) {
        close(savedStderr); close(pipefd[0]); close(pipefd[1]);
        return {kRanAbnormally3139, {}};
    }
    close(pipefd[1]);  // close write end in parent

    bezel::Driver driver;
    bezel::Dependency dep;
    dep.metadataPath = lObj.path;
    dep.objectPath = lObj.path;
    std::vector<bezel::Dependency> deps{dep};
    bool compileOk = bezel::test::compileFileForTest(
        driver, aSrc.path, aExe.path, deps).success;

    // Restore stderr
    dup2(savedStderr, STDERR_FILENO);
    close(savedStderr);

    // Read captured stderr
    std::string diag;
    char buf[256];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        diag += buf;
    }
    close(pipefd[0]);

    if (!compileOk) return {kCompileRejected3139, diag};
    int status = std::system(aExe.path.c_str());
    return {(status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status)
                                                : kRanAbnormally3139, {}};
}

// The LOCAL twin: one module, same program text, same verdict encoding.
int compileAndRun3139(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3139l_");
    TempFile3139 aSrc((dir / (stem + ".bzl")).string());
    TempFile3139 aExe((dir / stem).string());
    writeSource3139(aSrc.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, aSrc.path, aExe.path).success)
        return kCompileRejected3139;
    int status = std::system(aExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status)
                                               : kRanAbnormally3139;
}

// Local twin with diagnostic capture.  Returns {exit_status, captured_stderr}.
std::pair<int, std::string> compileLocalAndGetDiag3139(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3139l_");
    TempFile3139 aSrc((dir / (stem + ".bzl")).string());
    TempFile3139 aExe((dir / stem).string());
    writeSource3139(aSrc.path, src);

    int pipefd[2];
    if (pipe(pipefd) < 0) return {kRanAbnormally3139, {}};
    std::fflush(nullptr);
    int savedStderr = dup(STDERR_FILENO);
    if (savedStderr < 0) { close(pipefd[0]); close(pipefd[1]); return {kRanAbnormally3139, {}}; }
    if (dup2(pipefd[1], STDERR_FILENO) < 0) {
        close(savedStderr); close(pipefd[0]); close(pipefd[1]);
        return {kRanAbnormally3139, {}};
    }
    close(pipefd[1]);

    bezel::Driver driver;
    bool compileOk = bezel::test::compileFileForTest(driver, aSrc.path, aExe.path).success;

    dup2(savedStderr, STDERR_FILENO);
    close(savedStderr);

    std::string diag;
    char buf[256];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        diag += buf;
    }
    close(pipefd[0]);

    if (!compileOk) return {kCompileRejected3139, diag};
    int status = std::system(aExe.path.c_str());
    return {(status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status)
                                                : kRanAbnormally3139, {}};
}

// The dependency. Every declaration here names a built-in interface that has no
// user TypeSymbol, which is precisely what could not cross the boundary.
const char* kLib3139 =
    "module LibF1_3139; "
    // §18.5 / stdlib §2.18: a user type is format-spec-admissible only by
    // implementing `IFormattable`.
    "public class Money3139: IFormattable "
    "  private _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public method ToString(format: string): string => \"MX\"; "
    "end Money3139; "
    // §14.2: `IComparable<T>` — a builtin the compiler DOES synthesize a
    // TypeSymbol for (#1115/#2776), so it rides `baseTypes` on both sides. The
    // gates below read the AST anyway, which is what broke.
    "public class Rank3139: IComparable<Rank3139> "
    "  private _r: int; "
    "  public constructor(r: int) _r := r; end constructor; "
    "  public method Rank(): int => _r; "
    "  public method CompareTo(other: Rank3139): int => _r - other.Rank(); "
    "end Rank3139; "
    // The negative control: no conformance at all.
    "public class Plain3139 "
    "  private _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public method V(): int => _v; "
    "end Plain3139; "
    // fix #2772 item 2: a class that IMPORTS `IComparable<Other>` with a
    // wrong type argument.  #3139's symbol-side fallback admits any class
    // declaring IComparable when baseTypeRefs is empty on the import side;
    // this library entry provides a baseTypeRefs entry with a MISMATCHED
    // argument so the strict substitution check can fire even when the symbol
    // would admit via declaresBuiltinInterface.
    "public class Other3139 "
    "  public constructor() end constructor; "
    "end Other3139; "
    "public class Wrong3139: IComparable<Other3139> "
    "  private _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public method V(): int => _v; "
    "  public method CompareTo(other: Other3139): int => 0; "
    "end Wrong3139; ";

// The same three declarations, for the local twin.
const char* kLocalDecls3139 =
    "public class Money3139: IFormattable "
    "  private _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public method ToString(format: string): string => \"MX\"; "
    "end Money3139; "
    "public class Rank3139: IComparable<Rank3139> "
    "  private _r: int; "
    "  public constructor(r: int) _r := r; end constructor; "
    "  public method Rank(): int => _r; "
    "  public method CompareTo(other: Rank3139): int => _r - other.Rank(); "
    "end Rank3139; "
    "public class Plain3139 "
    "  private _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public method V(): int => _v; "
    "end Plain3139; ";

std::string importer3139(const std::string& body) {
    return "module AppF1_3139; import LibF1_3139; class C3139 "
           "  public static method Main(): int " + body +
           "  end Main; end C3139; ";
}

std::string local3139(const std::string& body) {
    return std::string("module AppF1_3139; ") + kLocalDecls3139 +
           "class C3139 "
           "  public static method Main(): int " + body +
           "  end Main; end C3139; ";
}

// §18.5: a format spec on an `IFormattable` operand dispatches to its
// `ToString(format)`. "MX" has length 2, so 42 distinguishes the taken branch
// from any degraded value.
const char* kFormatBody3139 =
    "    using var m := new Money3139(5); "
    "    return (if $\"{m:X}\" = \"MX\" then 42 else 91); ";

// §14.2 / #1195: `Array<T>.Sort()` requires `T: IComparable<T>`. 3,1,2 sorted
// ascending is 1,2,3, encoded base-4 (1*16 + 2*4 + 3 = 27) so the 8-bit
// WEXITSTATUS mask is never involved.
const char* kSortBody3139 =
    "    using var a := new Rank3139(3); "
    "    using var b := new Rank3139(1); "
    "    using var c := new Rank3139(2); "
    "    var arr: Array<Rank3139> := [a, b, c]; "
    "    arr.Sort(); "
    "    return arr[0].Rank() * 16 + arr[1].Rank() * 4 + arr[2].Rank(); ";

// §14.2 / #1198: `Min()` without a selector over a NON-comparable element must
// be rejected — the direction the imported case got backwards.
const char* kBadMinBody3139 =
    "    using var a := new Plain3139(7); "
    "    using var b := new Plain3139(3); "
    "    var arr: Array<Plain3139> := [a, b]; "
    "    return arr.Min().V(); ";

// fix #2772 item 2: `Sort()` on an IMPORTED class whose baseTypeRefs carries
// an IComparable base with a WRONG type argument.  With the #3139 fallback
// guarding against empty baseTypeRefs, the #2772 substitution check fires for
// this case and rejects correctly.
const char* kSortWrongArgBody3139 =
    "    using var a := new Wrong3139(3); "
    "    using var b := new Wrong3139(1); "
    "    var arr: Array<Wrong3139> := [a, b]; "
    "    arr.Sort(); "
    "    return 7; ";

}  // namespace

// ===========================================================================
// §18.5 — a format spec on an IMPORTED `: IFormattable` operand
// ===========================================================================

// The headline. `declaresIFormattable` walked each level's `ast::TypeDecl`
// base list, so an imported `class Money3139: IFormattable` answered false and
// §18.5 rejected the hole: "`Money3139` is neither". Measured before the fix:
// a compile reject (-1); after: 42.
BEZEL_TEST(xmod_builtin_iface_conformance_3139, format_spec_on_imported_iformattable_spec_18_5) {
    BEZEL_EXPECT_EQ(compileImportAndRun3139(kLib3139, importer3139(kFormatBody3139)), 42);
}

// The differential control: the SAME program in ONE module. It passed before
// the fix and must keep passing — so what the case above pins is the module
// boundary, not the shape.
BEZEL_TEST(xmod_builtin_iface_conformance_3139, control_local_iformattable_unchanged_spec_18_5) {
    BEZEL_EXPECT_EQ(compileAndRun3139(local3139(kFormatBody3139)), 42);
}

// §18.5's negative arm survives the fix: a plain imported class with NO
// conformance still must not admit a format spec. A fix that made
// `declaresBuiltinInterface` blanket-true for imports would pass the case above
// and fail this one.
BEZEL_TEST(xmod_builtin_iface_conformance_3139, reject_format_spec_on_imported_plain_class_spec_18_5) {
    BEZEL_EXPECT_EQ(compileImportAndRun3139(kLib3139, importer3139(
        "    using var p := new Plain3139(5); "
        "    return (if $\"{p:X}\" = \"x\" then 42 else 91); ")),
        kCompileRejected3139);
}

// ===========================================================================
// §14.2 — `Array<T>.Sort()` over an IMPORTED `: IComparable<T>` element
// ===========================================================================

// The #1195 gate scanned the element's `ast::TypeDecl` base list for
// "IComparable" and left `isComparableElem` false when there was none, so
// `Array<T>.Sort()` over an imported conforming element was rejected with
// "requires T to implement IComparable<T> (§3 / #472)". Measured before: -1;
// after: 27.
BEZEL_TEST(xmod_builtin_iface_conformance_3139, sort_over_imported_icomparable_element_spec_14_2) {
    BEZEL_EXPECT_EQ(compileImportAndRun3139(kLib3139, importer3139(kSortBody3139)), 27);
}

// The differential control for the §14.2 arm.
BEZEL_TEST(xmod_builtin_iface_conformance_3139, control_local_icomparable_sort_unchanged_spec_14_2) {
    BEZEL_EXPECT_EQ(compileAndRun3139(local3139(kSortBody3139)), 27);
}

// #2776's `Min()` path stays green. It already worked (the loader gives an
// imported occurrence of the synthesized `IComparable` symbol the same
// identity), and the conversion of the two AST gates must not disturb it.
BEZEL_TEST(xmod_builtin_iface_conformance_3139, min_over_imported_icomparable_element_spec_14_2) {
    BEZEL_EXPECT_EQ(compileImportAndRun3139(kLib3139, importer3139(
        "    using var a := new Rank3139(7); "
        "    using var b := new Rank3139(3); "
        "    var arr: Array<Rank3139> := [a, b]; "
        "    return arr.Min().Rank(); ")), 3);
}

// ===========================================================================
// §14.2 — the OVER-ACCEPT direction (the one codegen IR-verifier failure)
// ===========================================================================

// `Min()`'s verdict initialised to `true` and was only cleared inside `if (td)`,
// so an IMPORTED non-conforming element was ACCEPTED — sema then types the
// result as T and codegen has no `CompareTo` to dispatch through. Both before
// and after the fix the program must be REJECTED; before, the reject came from
// a downstream #2770 backstop rather than from this gate, so the arm is written
// as "rejected", which is the property that actually matters.
BEZEL_TEST(xmod_builtin_iface_conformance_3139, reject_non_comparable_imported_element_spec_14_2) {
    BEZEL_EXPECT_EQ(compileImportAndRun3139(kLib3139, importer3139(kBadMinBody3139)),
                    kCompileRejected3139);
}

// The local twin of the same reject.
BEZEL_TEST(xmod_builtin_iface_conformance_3139, control_reject_non_comparable_local_element_spec_14_2) {
    BEZEL_EXPECT_EQ(compileAndRun3139(local3139(kBadMinBody3139)),
                    kCompileRejected3139);
}

// ===========================================================================
// §2.3 / §18.5 — a LOCAL behaviour change, made deliberately
// ===========================================================================

// #3139 replaces §18.5's name match ("a base whose simple name is
// `IFormattable`") with an IDENTITY test against the AUTO-IMPORTED interface.
// §2.3 permits a user declaration to shadow that name, and §18.5 is explicit
// that the opt-in is the auto-imported one ("a **user** type opts in by
// implementing the auto-imported interface `IFormattable`"), so a class
// implementing a SHADOW is not formattable and a spec on it is a §18.5 error.
//
// This is the ONE case where a local answer changes, and it is spec-correct.
// Measured: `Fake` below returned 77 before the fix and is rejected after.
//
// The exclusion arrives by a DIFFERENT route than the `IComparable` family's.
// There, `TypeSymbol::isSynthesizedBuiltinInterface` separates the built-in
// from a same-named user type. `IFormattable` has no TypeSymbol at all, so the
// shadow is excluded because a user-declared base HAS a declaration and
// therefore lands in `baseTypes`, never in `nonSymbolBaseRefs` — the only
// place the non-symbol built-ins live. Two mechanisms, one rule: identity, not
// name. The diagnostic text is pinned in
// tests/lit/builtin_iface_shadow_reject_3139.bzl.
BEZEL_TEST(xmod_builtin_iface_conformance_3139, local_shadowed_iformattable_spec_denied_spec_2_3_18_5) {
    BEZEL_EXPECT_EQ(compileAndRun3139(
        "module M3139S; "
        "interface IFormattable "
        "  method ToString(format: string): string; "
        "end IFormattable; "
        "class Fake3139: IFormattable "
        "  public method ToString(format: string): string => \"F\"; "
        "end Fake3139; "
        "class C3139 public static method Main(): int "
        "  using var f := new Fake3139(); "
        "  return (if $\"{f:X}\" = \"F\" then 77 else 0); "
        "end Main; end C3139; "), kCompileRejected3139);
}

// The other half, and the reason this is a narrowing rather than a ban: §2.3
// permits the shadow to EXIST and be used. Only the §18.5 format-spec
// admission is denied — the same declaration, the same call written as an
// ordinary method call, still compiles and still returns 77.
BEZEL_TEST(xmod_builtin_iface_conformance_3139, local_shadowed_iformattable_still_usable_spec_2_3) {
    BEZEL_EXPECT_EQ(compileAndRun3139(
        "module M3139S2; "
        "interface IFormattable "
        "  method ToString(format: string): string; "
        "end IFormattable; "
        "class Fake3139: IFormattable "
        "  public method ToString(format: string): string => \"F\"; "
        "end Fake3139; "
        "class C3139 public static method Main(): int "
        "  using var f := new Fake3139(); "
        "  var g: IFormattable := f; "
        "  return (if g.ToString(\"X\") = \"F\" then 77 else 0); "
        "end Main; end C3139; "), 77);
}

// ===========================================================================
// §9.8 — the interlock #3139 left closed, and #3140 opened
// ===========================================================================

// #3139 gave the §9.8 conformance question a symbol-side answer but held the
// surface shut behind `sema::serializationSurfaceIsEmittable`, because the §9.8
// EMITTERS still took their component list and wire names from the
// `ast::TypeDecl` — families F2 (#3140) and F3 (#3141) of the same umbrella.
// Admitting the surface on conformance alone would have replaced a truthful
// reject with SILENTLY WRONG BYTES: measured with the interlock removed and
// nothing else changed, an importer's `Wrap(P: Pt, N: int)` over an imported
// serializable `Pt` round-tripped to 244 for an expected 534, and
// `Pt.Serialize(s)` compiled to a degraded zero. That is the #1462 failure mode.
//
// #3140 (family F2) supplied what was missing — `emitRecordSerialize` /
// `emitRecordDeserialize` now walk `collectRecordStorage` and read each
// component's `serialized` clause through `sema::serializedComponentClause`,
// which rides `MemberSymbol` on both sides — and DELETED the interlock. So this
// arm flips, exactly as its previous revision said it would: from
// `kCompileRejected3139` to 34, the same value its local twin below has always
// had. The wider F2 evidence (the `serialized skip`/rename wire-name oracle,
// its vacuity control, `serialized optional`, the body-record form, and the
// over-acceptance controls) is in
// tests/module/xmod_record_serialization_3140_test.cpp.
BEZEL_TEST(xmod_builtin_iface_conformance_3139, imported_serialization_surface_round_trips_spec_9_8) {
    const char* lib =
        "module LibSer3139; "
        "public record Pt3139(X: int, Y: int): ISerializable, IDeserializable "
        "end Pt3139; ";
    const std::string app =
        "module AppSer3139; import LibSer3139; import System.Serialization; "
        "class C3139 public static method Main(): int "
        "  var p := new Pt3139(3, 4); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var p2 := Pt3139.Deserialize(d); "
        "  return p2.X * 10 + p2.Y; "
        "end Main; end C3139; ";
    BEZEL_EXPECT_EQ(compileImportAndRun3139(lib, app), 34);
}

// The LOCAL §9.8 round-trip is untouched by any of this — the same declaration
// and the same body, in one module, still round-trips to 34. It was the arm
// that would have gone red if the interlock had been written as a blanket
// "§9.8 is off"; it is now the differential control proving the imported arm
// above reaches the SAME value, so the module boundary was the whole
// difference.
BEZEL_TEST(xmod_builtin_iface_conformance_3139, control_local_serialization_roundtrip_spec_9_8) {
    const std::string src =
        "module AppSerL3139; import System.Serialization; "
        "public record Pt3139(X: int, Y: int): ISerializable, IDeserializable "
        "end Pt3139; "
        "class C3139 public static method Main(): int "
        "  var p := new Pt3139(3, 4); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var p2 := Pt3139.Deserialize(d); "
        "  return p2.X * 10 + p2.Y; "
        "end Main; end C3139; ";
    BEZEL_EXPECT_EQ(compileAndRun3139(src), 34);
}

// ===========================================================================
// §14.2 — imported class with WRONG IComparable type argument (#2772 item 2)
// ===========================================================================

// fix #2772 item 2 (spec §14.2, stdlib §4.4, v0.276): the class arm of the
// IComparable gate must also check the TYPE ARGUMENT — the conformance
// requirement is T: IComparable<T>, not just T: IComparable<Any>.  An
// imported `class Wrong3139: IComparable<Other3139>` is NOT comparable by
// Wrong3139's own identity; only Wrong3139: IComparable<Wrong3139> satisfies
// the self-recurrence.
//
// Three directions are pinned:
//   1. the LOCAL wrong-arg class is REJECTED (the #2772 item 2 fix)
//   2. the IMPORTED wrong-arg class is REJECTED (the substitution check
//      fires via baseTypeRefs even when declaresBuiltinInterface would admit)
//   3. the CORRECT-arg imported class (Rank3139, from kLib3139 above) is
//      STILL ACCEPTED (the #3139 fallback is not disturbed)
//
// MECHANISM (confirmed by tracing src/module/ModuleLoader.cpp L862-878):
// IComparable has a synthesized TypeSymbol (it is NOT in builtinInterfaceBases,
// which is only for interfaces with NO TypeSymbol — ISerializable, IFormattable,
// IEnumerable<T>, etc.).  On the import side, the class's base
// IComparable<Other3139> goes through td.baseTypes → convert() (L862-865):
// convert() resolves the base's declaration to the synthesized IComparable
// TypeSymbol (L413-416: "ref->declaration =
// ctx.builtinInterfaceSym(mt.namedName)" for synthesized interface names),
// so it passes the "Named && declaration" guard and is pushed to BOTH
// baseTypeRefs AND baseTypes.  Therefore baseTypeRefsHasIComparableEntry is
// true, the declaresBuiltinInterface fallback is NOT consulted, and the
// substitution check fires: IComparable<Other3139> (substituted with the
// empty class-level map) vs Wrong3139 → not equal → REJECTED.
//
// The ModuleLoader comment at L847 ("builtinInterfaceBases ... populated from
// their own MetaType fields, NOT from the baseTypes array") refers to the
// nonSymbolBaseRefs channel for interfaces WITHOUT a TypeSymbol; IComparable
// has a TypeSymbol so it rides the td.baseTypes path.

// LOCAL CONTROL: a local wrong-arg class is correctly rejected (the #2772 fix).
// Asserts the specific §14.2 diagnostic, not just the exit code.
BEZEL_TEST(xmod_builtin_iface_conformance_3139,
           sort_local_wrong_icomp_type_arg_rejected_spec_14_2) {
    const char* src =
        "module AppWL; "
        "class OtherWL "
        "  public constructor() end constructor; "
        "end OtherWL; "
        "class WrongWL: IComparable<OtherWL> "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method V(): int => _v; "
        "  public method CompareTo(other: OtherWL): int => 0; "
        "end WrongWL; "
        "class C public static method Main(): int "
        "  using var a := new WrongWL(3); "
        "  using var b := new WrongWL(1); "
        "  var arr: Array<WrongWL> := [a, b]; "
        "  arr.Sort(); "
        "  return 7; "
        "end Main; end C; ";
    auto [status, diag] = compileLocalAndGetDiag3139(src);
    BEZEL_EXPECT_EQ(status, kCompileRejected3139);
    BEZEL_EXPECT_TRUE(diag.find("Sort() requires T: IComparable<T>") != std::string::npos);
    BEZEL_EXPECT_TRUE(diag.find("WrongWL does not implement IComparable<WrongWL>") != std::string::npos);
}

// IMPORTED: an imported wrong-arg class is rejected via the substitution check
// (IComparable has a synthesized TypeSymbol, so it goes through td.baseTypes
// and is in baseTypeRefs on the import side; baseTypeRefsHasIComparableEntry
// is true; declaresBuiltinInterface fallback is NOT consulted; substitution
// check fires: IComparable<Other3139> != Wrong3139 → REJECTED).
// Asserts the specific §14.2 diagnostic, not just the exit code.
BEZEL_TEST(xmod_builtin_iface_conformance_3139,
           sort_imported_wrong_icomp_type_arg_rejected_spec_14_2) {
    auto [status, diag] = compileImportAndGetDiag3139(
        kLib3139, importer3139(kSortWrongArgBody3139));
    BEZEL_EXPECT_EQ(status, kCompileRejected3139);
    BEZEL_EXPECT_TRUE(diag.find("Sort() requires T: IComparable<T>") != std::string::npos);
    BEZEL_EXPECT_TRUE(diag.find("Wrong3139 does not implement IComparable<Wrong3139>") != std::string::npos);
}

// IMPORTED CORRECT-ARG CONTROL: the existing #3139 guard — an imported class
// that DOES implement IComparable<Self> correctly must still be ACCEPTED.
// This is the headline row from this file: the first test added in the
// pre-regression version of this branch, pinned at value 27.
BEZEL_TEST(xmod_builtin_iface_conformance_3139,
           sort_imported_correct_icomp_still_sorts_spec_14_2) {
    BEZEL_EXPECT_EQ(
        compileImportAndRun3139(kLib3139, importer3139(kSortBody3139)), 27);
}

}  // namespace bezel::module
