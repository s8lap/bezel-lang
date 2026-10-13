// Spec-derived regression test for issue #3194 — family M13 ("interface
// conformance witness inherited from an imported base") of the MemberSymbol
// seam, umbrella #3152.
//
// §11.2 (interface conformance and the `matches` rule, including "`owned` in
// interface signatures") / §2.8.2 clause 1 / §2.8.3 rule 4, v0.254.
// Audit: docs/proposals/papers/member-symbol-audit-2026-08-13.md, family M13 (rank 2).
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// `Resolver::checkAbstractImplementations` (src/sema/TypeResolution_Checks.cpp)
// resolves each interface obligation to a witness by walking the class chain,
// and that walk follows `c->baseTypes` — which CROSSES MODULES. So the chosen
// witness `impl` is an IMPORTED `MethodSymbol` whenever a local class satisfies
// an obligation through a method it inherits from an imported base, and an
// imported MethodSymbol carries no `ast::MethodDecl`.
//
// Three of the four §11.2 signature axes were keyed on that decl and therefore
// silently did not run:
//
//   * the `owned`-return match  (`if (implDecl && imRetOwned != implRetOwned)`)
//   * the return-type match     (`implRetNode = implDecl ? ... : nullptr`)
//   * the parameter-type match  (`ct = resolvedOf(cp->declType)`, null for an
//                                imported ParameterSymbol — its type rides
//                                `resolvedDeclType`, exactly as the INTERFACE
//                                side's own #2373 fallback already knew)
//
// Only the parameter-MODIFIER axis kept working, because it reads
// `ParameterSymbol::modifier`, which the loader does populate.
//
// Three comments in that function asserted the opposite — "`impl` is always a
// LOCAL class member (obligation witnesses only ever come from `t`'s own
// module's BFS above)" — while the BFS 60 lines above them walked
// `c->baseTypes`. The sibling comment in the same file's §11.8 static loop
// states the correct thing. Those comments are corrected in the same commit.
//
// ── MEASURED, on the fix's parent ──────────────────────────────────────────
//
// `bezelc -c lib.bzl -o lib.o` then `bezelc --import lib.o app.bzl`, each
// against a character-identical single-module twin:
//
//   axis          program                                twin        imported
//   return type   `Get(): int` vs iface `Get(): string`   §11.2 error compiles,
//                                                                     SIGSEGV
//   param type    `Take(x: int)` vs `Take(x: string)`     §11.2 error compiles,
//                                                                     returns a garbage int (225 here, 241 under review — layout-dependent)
//   `owned` ret   `Make(): owned T` vs iface `Make(): T`  §11.2 error compiles,
//                                                                     LSan: 16-byte
//                                                                     direct leak
//
// The audit frames the `owned` bind as unwinnable in both directions — dropping
// `owned` off an inherited implementation leaks (the caller never records the
// transfer, exactly what LeakSanitizer measured above), tightening it double-
// disposes. §11.2 supplies the third answer, and it is the one the
// single-module twin already gives: REJECT the mismatch.
//
// ── WHY THE CONTROLS ARE HALF THIS FILE ────────────────────────────────────
//
// This fix turns three skipped checks ON for a population they never saw. The
// failure mode of that shape is a FALSE REJECT of a correct program, and the
// specific hazard is type identity: an imported witness's types are TypeRefs
// the loader reconstructed, compared against TypeRefs the local interface
// resolved from source. T5/T6 run twelve signature shapes — int, string, bool,
// double, `int?`, a dependency class param, an `owned` class return, an inline
// array `[3]int`, a no-return method, an enum, a nullable class param and a
// `ref` param — with a CORRECT witness, in both the local-interface and
// imported-interface arrangements, and require all of them to compile and run.

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

struct TempFile3194 {
    std::string path;
    explicit TempFile3194(std::string p) : path(std::move(p)) {}
    ~TempFile3194() { std::remove(path.c_str()); }
};

void writeSource3194(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Compiled3194 {
    bool depOk = true;
    bool success = false;
    std::string err;
};

template <typename Fn>
std::string captureStderr3194(Fn&& fn) {
    namespace fs = std::filesystem;
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz3194_err_" + unique_stem("") + ".txt"))
            .string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    fn();
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    std::ifstream in(errPath);
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return out;
}

Compiled3194 compileImport3194(const std::string& libSrc,
                               const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3194_");
    TempFile3194 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3194 aObj((dir / (stem + "_a.o")).string());
    writeSource3194(aSrc.path, libSrc);
    Compiled3194 r;
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good()) {
            r.depOk = false;
            return r;
        }
    }
    TempFile3194 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3194 bExe((dir / (stem + "_b")).string());
    writeSource3194(bSrc.path, appSrc);
    r.err = captureStderr3194([&] {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        r.success =
            bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
                .success;
    });
    return r;
}

Compiled3194 compileLocal3194(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3194l_");
    TempFile3194 aSrc((dir / (stem + ".bzl")).string());
    TempFile3194 aExe((dir / stem).string());
    writeSource3194(aSrc.path, src);
    Compiled3194 r;
    r.err = captureStderr3194([&] {
        bezel::Driver driver;
        r.success =
            bezel::test::compileFileForTest(driver, aSrc.path, aExe.path)
                .success;
    });
    return r;
}

// Compile lib -> object, app -> exe, then RUN. -2 = dependency unavailable.
int compileImportAndRun3194(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3194r_");
    TempFile3194 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3194 aObj((dir / (stem + "_a.o")).string());
    writeSource3194(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good())
            return -2;
    }
    TempFile3194 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3194 bExe((dir / stem).string());
    writeSource3194(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
             .success)
        return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

bool mentions3194(const Compiled3194& c, const std::string& needle) {
    return c.err.find(needle) != std::string::npos;
}

// The dependency's declarations for the three MISMATCH cases.
const char* kBadBody3194 =
    "public class ThingM3194 "
    "  public v: int := 5; "
    "end ThingM3194; "
    "public class BaseM3194 "
    "  public method Get(): int "
    "    return 7; "
    "  end Get; "
    "  public method Take(x: int): int "
    "    return x + 1; "
    "  end Take; "
    "  public method Make(): owned ThingM3194 "
    "    return new ThingM3194(); "
    "  end Make; "
    "  public method Self(): BaseM3194 "
    "    return this; "
    "  end Self; "
    "end BaseM3194; ";

// Run one app body BOTH ways and require BOTH to reject with `needle`. The
// single-module twin is the oracle: it is the spelling §11.2 already rejects,
// and the imported build must not answer differently (§2.8.3 rule 4).
void expectBothReject3194(const std::string& appBody,
                          const std::string& needle) {
    Compiled3194 twin =
        compileLocal3194("module VS3194; " + std::string(kBadBody3194) + appBody);
    BEZEL_EXPECT_TRUE(!twin.success);
    BEZEL_EXPECT_TRUE(mentions3194(twin, needle));
    BEZEL_EXPECT_TRUE(mentions3194(twin, "11.2"));

    Compiled3194 imported =
        compileImport3194("module VL3194; " + std::string(kBadBody3194),
                          "module VA3194; import VL3194; " + appBody);
    if (!imported.depOk) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(!imported.success);
    BEZEL_EXPECT_TRUE(mentions3194(imported, needle));
    BEZEL_EXPECT_TRUE(mentions3194(imported, "11.2"));
}

}  // namespace

// ===========================================================================
// T1 — §11.2 RETURN TYPE. `Get(): int` inherited from the imported base
// against an interface obligation `Get(): string`. This is the axis whose
// false accept was MEMORY-UNSAFE: the itable slot held an int-returning
// function and the caller drove the returned 7 through `bz.string` (SIGSEGV,
// measured). §11.2 admits no covariance on returns.
// ===========================================================================
BEZEL_TEST(xmod_iface_witness_from_imported_base_3194,
           inherited_witness_return_type_mismatch_rejected_11_2) {
    expectBothReject3194(
        "public interface IRet3194 "
        "  method Get(): string; "
        "end IRet3194; "
        "public class DRet3194: BaseM3194, IRet3194 "
        "end DRet3194; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new DRet3194(); "
        "    var i: IRet3194 := d; "
        "    var s: string := i.Get(); "
        "    return s.Length; "
        "  end Main; "
        "end C;",
        "return type");
}

// ===========================================================================
// T2 — §11.2 PARAMETER TYPE. `Take(x: int)` against `Take(x: string)`. The
// imported ParameterSymbol has no `declType`; its type rides
// `resolvedDeclType`, the same channel the INTERFACE side of this comparison
// has read since #2373. Measured false accept: the program compiled and
// returned 225 — a truncated string pointer plus one.
// ===========================================================================
BEZEL_TEST(xmod_iface_witness_from_imported_base_3194,
           inherited_witness_param_type_mismatch_rejected_11_2) {
    expectBothReject3194(
        "public interface IPar3194 "
        "  method Take(x: string): int; "
        "end IPar3194; "
        "public class DPar3194: BaseM3194, IPar3194 "
        "end DPar3194; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new DPar3194(); "
        "    var i: IPar3194 := d; "
        "    return i.Take(\"hello\"); "
        "  end Main; "
        "end C;",
        "on parameter 1");
}

// ===========================================================================
// T3 — §11.2 "`owned` in interface signatures", the LOOSENING direction: the
// implementation returns `owned T`, the interface does not. §11.2 reason 1:
// the caller never records the transfer. LeakSanitizer measured exactly that
// on the pre-fix build — "Direct leak of 16 byte(s) ... in
// bz$VL3194.BaseM3194$Make".
// ===========================================================================
BEZEL_TEST(xmod_iface_witness_from_imported_base_3194,
           inherited_witness_owned_return_dropped_rejected_11_2) {
    expectBothReject3194(
        "public interface IOwn3194 "
        "  method Make(): ThingM3194; "
        "end IOwn3194; "
        "public class DOwn3194: BaseM3194, IOwn3194 "
        "end DOwn3194; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new DOwn3194(); "
        "    var i: IOwn3194 := d; "
        "    var t := i.Make(); "
        "    return t.v; "
        "  end Main; "
        "end C;",
        "`owned` return modifier");
}

// ===========================================================================
// T4 — the same axis, TIGHTENING direction: the interface declares `owned`,
// the inherited implementation does not. The audit's framing is that both
// directions are wrong (a leak one way, a double dispose the other); §11.2's
// answer is neither — the conformance itself is rejected. Pinning both
// directions is what proves the check is a real equality and not a one-sided
// guard that happens to catch the measured case.
// ===========================================================================
BEZEL_TEST(xmod_iface_witness_from_imported_base_3194,
           inherited_witness_owned_return_added_rejected_11_2) {
    expectBothReject3194(
        "public interface IOwn2_3194 "
        "  method Self(): owned BaseM3194; "
        "end IOwn2_3194; "
        "public class DOwn2_3194: BaseM3194, IOwn2_3194 "
        "end DOwn2_3194; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new DOwn2_3194(); "
        "    var i: IOwn2_3194 := d; "
        "    return 0; "
        "  end Main; "
        "end C;",
        "`owned` return modifier");
}

// ===========================================================================
// T5 — OVER-REJECTION CONTROL, the one this fix most needs. Twelve signature
// shapes, all CORRECTLY matched, all satisfied through the imported base:
// int / string / bool / double / `int?` / a dependency class parameter /
// an `owned` class return / an inline array `[3]int` / a no-return method /
// an enum / a nullable class parameter / a `ref` parameter. Every one of them
// compares a loader-reconstructed TypeRef against one the LOCAL interface
// resolved from source, which is precisely where a newly-enabled check would
// produce a false reject. Must compile, link and run; `Main` returns 3 (the
// three behaviours it actually exercises through the interface).
// ===========================================================================
BEZEL_TEST(xmod_iface_witness_from_imported_base_3194,
           correct_inherited_witness_grid_still_accepted_11_2) {
    const std::string libBody =
        "public class Th3194 "
        "  public v: int := 5; "
        "end Th3194; "
        "public enum Col3194 Red, Green; end Col3194; "
        "public class GBase3194 "
        "  public method AInt(x: int): int "
        "    return x + 1; "
        "  end AInt; "
        "  public method BStr(s: string): string "
        "    return s; "
        "  end BStr; "
        "  public method CBool(b: bool): bool "
        "    return b; "
        "  end CBool; "
        "  public method DDbl(d: double): double "
        "    return d; "
        "  end DDbl; "
        "  public method ENul(x: int?): int? "
        "    return x; "
        "  end ENul; "
        "  public method FThing(t: Th3194): int "
        "    return t.v; "
        "  end FThing; "
        "  public method GOwned(): owned Th3194 "
        "    return new Th3194(); "
        "  end GOwned; "
        "  public method HArr(a: [3]int): int "
        "    return a[0]; "
        "  end HArr; "
        "  public method IVoid(x: int) "
        "    var y := x; "
        "  end IVoid; "
        "  public method JEnum(c: Col3194): Col3194 "
        "    return c; "
        "  end JEnum; "
        "  public method KNul(t: Th3194?): bool "
        "    return t = null; "
        "  end KNul; "
        "  public method LRef(ref x: int): int "
        "    x := x + 1; "
        "    return x; "
        "  end LRef; "
        "end GBase3194; ";
    const std::string ifaceDecl =
        "public interface IAll3194 "
        "  method AInt(x: int): int; "
        "  method BStr(s: string): string; "
        "  method CBool(b: bool): bool; "
        "  method DDbl(d: double): double; "
        "  method ENul(x: int?): int?; "
        "  method FThing(t: Th3194): int; "
        "  method GOwned(): owned Th3194; "
        "  method HArr(a: [3]int): int; "
        "  method IVoid(x: int); "
        "  method JEnum(c: Col3194): Col3194; "
        "  method KNul(t: Th3194?): bool; "
        "  method LRef(ref x: int): int; "
        "end IAll3194; ";
    const std::string appTail =
        "public class DAll3194: GBase3194, IAll3194 "
        "end DAll3194; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new DAll3194(); "
        "    var i: IAll3194 := d; "
        "    var n: int := 0; "
        "    if i.AInt(1) = 2 then n := n + 1; end if; "
        "    if i.CBool(true) then n := n + 1; end if; "
        "    var r: int := 10; "
        "    if i.LRef(ref r) = 11 then n := n + 1; end if; "
        "    return n; "
        "  end Main; "
        "end C;";

    int twin = compileLocal3194("module VS3194G; " + libBody + ifaceDecl +
                                appTail)
                       .success
                   ? 0
                   : -1;
    BEZEL_EXPECT_EQ(twin, 0);
    int imported = compileImportAndRun3194(
        "module VL3194G; " + libBody,
        "module VA3194G; import VL3194G; " + ifaceDecl + appTail);
    if (imported == -2) return;
    BEZEL_EXPECT_EQ(imported, 3);
}

// ===========================================================================
// T6 — OVER-REJECTION CONTROL, BOTH sides imported: the interface is declared
// in the dependency too, so `im` and `impl` are both loader-reconstructed
// symbols and BOTH sides of every comparison are reconstructed TypeRefs. This
// is the arrangement neither #2373 (imported interface, LOCAL witness) nor T5
// (local interface, imported witness) covers.
// ===========================================================================
BEZEL_TEST(xmod_iface_witness_from_imported_base_3194,
           correct_witness_with_imported_interface_too_still_accepted_11_2) {
    const std::string libBody =
        "public class Th3194B "
        "  public v: int := 5; "
        "end Th3194B; "
        "public interface IAll3194B "
        "  method AInt(x: int): int; "
        "  method BStr(s: string): string; "
        "  method GOwned(): owned Th3194B; "
        "  method KNul(t: Th3194B?): bool; "
        "  method LRef(ref x: int): int; "
        "end IAll3194B; "
        "public class GBase3194B "
        "  public method AInt(x: int): int "
        "    return x + 1; "
        "  end AInt; "
        "  public method BStr(s: string): string "
        "    return s; "
        "  end BStr; "
        "  public method GOwned(): owned Th3194B "
        "    return new Th3194B(); "
        "  end GOwned; "
        "  public method KNul(t: Th3194B?): bool "
        "    return t = null; "
        "  end KNul; "
        "  public method LRef(ref x: int): int "
        "    x := x + 1; "
        "    return x; "
        "  end LRef; "
        "end GBase3194B; ";
    const std::string appTail =
        "public class DAll3194B: GBase3194B, IAll3194B "
        "end DAll3194B; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new DAll3194B(); "
        "    var i: IAll3194B := d; "
        "    var n: int := 0; "
        "    if i.AInt(1) = 2 then n := n + 1; end if; "
        "    if i.KNul(null) then n := n + 1; end if; "
        "    var r: int := 10; "
        "    if i.LRef(ref r) = 11 then n := n + 1; end if; "
        "    return n; "
        "  end Main; "
        "end C;";
    int imported = compileImportAndRun3194(
        "module VL3194B; " + libBody,
        "module VA3194B; import VL3194B; " + appTail);
    if (imported == -2) return;
    BEZEL_EXPECT_EQ(imported, 3);
}

// ===========================================================================
// T7 — OVER-REJECTION CONTROL for the §11.2 nullability WIDENING §11.2 admits
// on PARAMETERS ("impl may declare T? where the interface requires T"). The
// newly-enabled parameter-type comparison must keep that allowance for an
// imported witness, not just for a local one — otherwise the fix trades a
// false accept for a false reject of a spelling §11.2 names explicitly.
// ===========================================================================
BEZEL_TEST(xmod_iface_witness_from_imported_base_3194,
           param_nullability_widening_still_admitted_for_imported_witness_11_2) {
    const std::string libBody =
        "public class GBase3194W "
        "  public method Wide(x: int?): int "
        "    if x = null then return 0; end if; "
        "    return 1; "
        "  end Wide; "
        "end GBase3194W; ";
    const std::string appTail =
        "public interface IW3194 "
        "  method Wide(x: int): int; "
        "end IW3194; "
        "public class DW3194: GBase3194W, IW3194 "
        "end DW3194; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new DW3194(); "
        "    var i: IW3194 := d; "
        "    return i.Wide(5); "
        "  end Main; "
        "end C;";
    Compiled3194 twin =
        compileLocal3194("module VS3194W; " + libBody + appTail);
    BEZEL_EXPECT_TRUE(twin.success);
    int imported = compileImportAndRun3194(
        "module VL3194W; " + libBody,
        "module VA3194W; import VL3194W; " + appTail);
    if (imported == -2) return;
    BEZEL_EXPECT_EQ(imported, 1);
}

}  // namespace bezel::module
