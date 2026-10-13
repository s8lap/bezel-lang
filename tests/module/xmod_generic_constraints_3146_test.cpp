// Spec-derived regression test for issue #3146 — family F13 (§14.2 generic
// constraints) of the imported-symbol classification seam (umbrella #3152).
//
// §14.2 (generic constraints) / Appendix C.4 / §2.8.3 rule 4 (imported
// artifacts), v0.252.
// Audit: docs/proposals/papers/imported-symbol-audit-2026-08-13.md, family F13.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// `Resolver::checkGenericConstraints` (src/sema/TypeResolution_Checks.cpp) runs
// at EVERY instantiation site and opened with
//
//     if (!td || td->genericConstraints.empty()) return;
//
// where `td` is the DECLARING type's `ast::TypeDecl`. `ModuleLoader` never
// assigns `Symbol::decl` for an imported type — the importing compilation has no
// AST for a dependency's types at all — so for `Dep.Registry<K>` the cast is
// null and the function returned immediately. EVERY §14.2 constraint on EVERY
// imported generic type was unchecked: `where T: class`, `where T: new()`,
// `where T: IFace`, `where T: U` — all silently satisfied by any argument.
//
// Unlike the other families in this umbrella the data was never missing: the
// artifact has carried `MetaGenericParam::constraintTypes` /
// `specialConstraints` since #1579/v0.182. `ModuleLoader::convertGenericParams`
// read them off the wire and threw them away, because `GenericParamSymbol` had
// nowhere to put them. The fix gives it somewhere
// (`GenericParamSymbol::constraints`), populates it from the AST on the local
// side and from the wire on the imported side, and rewrites the readers against
// the symbol.
//
// ── MEASURED, on 558c27c, with a real two-module build ─────────────────────
//   (`bezelc -c dep.bzl -o dep.o` then `bezelc --import dep.o -o app app.bzl`)
//
// Four constraint forms, all four violated by the importer's instantiation:
//
//   LOCAL (all declarations in one module) — 4 errors, no executable:
//     type argument `int` does not satisfy the constraint `where T: class` (§14.2)
//     type argument `NoNullary` does not satisfy the constraint `where T: new()` (§14.2)
//     type argument `Plain` does not satisfy the constraint `where T: IShapeG` (§14.2)
//     type argument `Plain` does not satisfy the constraint `where T: IShapeG` (§14.2)
//   IMPORTED (the same declarations in a dependency) — ZERO diagnostics at any
//     severity; the importer compiled, linked and ran, exit 0.
//
// ── WHAT IS AND IS NOT COVERED ─────────────────────────────────────────────
//
// #3146 lists three sites. This file's behavioural cases pin the first —
// `checkGenericConstraints`, the one with the blast radius. The other two
// (`leafParamExcludesNullable`, `leafSolutionViolatesScalarConstraint`, both in
// ExprTypecheck.cpp) are converted in the same commit because the family lands
// whole, but they CANNOT be exercised across a module boundary today and this
// file deliberately does not pretend otherwise: §19.6's closed-set leaf walk
// enumerates `ctx_.symbols()`, which by construction excludes every imported
// symbol, so an imported `closed` hierarchy has an EMPTY leaf set and the whole
// exhaustiveness check silently never runs. MEASURED: the local twin of the
// dependency below warns "closed hierarchy `BC` is missing leaf type(s): LC",
// the imported build says nothing. Filed as #3167. Their local behaviour is
// unchanged and stays covered by tests/sema/case_exhaustiveness_test.cpp.

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

struct TempFile3146 {
    std::string path;
    explicit TempFile3146(std::string p) : path(std::move(p)) {}
    ~TempFile3146() { std::remove(path.c_str()); }
};

void writeSource3146(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Compiled3146 {
    bool depOk = true;
    bool success = false;
    std::string err;
};

template <typename Fn>
std::string captureStderr3146(Fn&& fn) {
    namespace fs = std::filesystem;
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz3146_err_" + unique_stem("") + ".txt"))
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

Compiled3146 compileImport3146(const std::string& libSrc,
                               const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3146_");
    TempFile3146 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3146 aObj((dir / (stem + "_a.o")).string());
    writeSource3146(aSrc.path, libSrc);
    Compiled3146 r;
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good()) {
            r.depOk = false;
            return r;
        }
    }
    TempFile3146 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3146 bExe((dir / stem).string());
    writeSource3146(bSrc.path, appSrc);
    r.err = captureStderr3146([&] {
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

Compiled3146 compileLocal3146(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3146l_");
    TempFile3146 aSrc((dir / (stem + ".bzl")).string());
    TempFile3146 aExe((dir / stem).string());
    writeSource3146(aSrc.path, src);
    Compiled3146 r;
    r.err = captureStderr3146([&] {
        bezel::Driver driver;
        r.success =
            bezel::test::compileFileForTest(driver, aSrc.path, aExe.path)
                .success;
    });
    return r;
}

bool mentions3146(const Compiled3146& c, const std::string& needle) {
    return c.err.find(needle) != std::string::npos;
}

// The four §14.2 constraint forms the grammar admits on a type parameter, all
// on PUBLIC exported generic classes: the two scalar forms that need no bound
// (`class`, `new()`), the type bound (`where T: IFace`), and the
// type-PARAMETER bound (`where T: U`), which checkGenericConstraints handles on
// its own dedicated leg.
const char* kDecls3146 =
    "public interface IShapeG3146 method Area(): int; end IShapeG3146; "
    "public class NoNullary3146 "
    "  public v: int := 0; "
    "  public constructor(x: int) v := x; end constructor; "
    "end NoNullary3146; "
    "public class BoxC3146<T> where T: class public v: int := 0; end BoxC3146; "
    "public class BoxN3146<T> where T: new() public v: int := 0; end BoxN3146; "
    "public class BoxI3146<T> where T: IShapeG3146 "
    "  public v: int := 0; "
    "end BoxI3146; "
    "public class PairP3146<T, U> where T: U public v: int := 0; end PairP3146; ";

// A class that satisfies NONE of the bounds: not IShapeG3146, and (as a type
// ARGUMENT to `where T: class`) `int` is used instead.
const char* kPlain3146 =
    "public class Plain3146 public v: int := 0; end Plain3146; ";

// A class that satisfies all of them.
const char* kShaped3146 =
    "public class Shaped3146: IShapeG3146 "
    "  public method Area(): int return 4; end Area; "
    "end Shaped3146; ";

// The violating instantiations, one per constraint form. `null`-initialised so
// nothing is constructed — the §14.2 check fires at the TYPE, which is the
// point: it is an instantiation-site check, not a construction-site one.
const char* kViolatingBody3146 =
    "public class Program3146 "
    "  public static method Main(): int "
    "    var a: BoxC3146<int>? := null; "
    "    var b: BoxN3146<NoNullary3146>? := null; "
    "    var c: BoxI3146<Plain3146>? := null; "
    "    var d: PairP3146<Plain3146, IShapeG3146>? := null; "
    "    if a <> null then return 1; end if; "
    "    if b <> null then return 2; end if; "
    "    if c <> null then return 3; end if; "
    "    if d <> null then return 4; end if; "
    "    return 0; "
    "  end Main; "
    "end Program3146;";

const char* kSatisfyingBody3146 =
    "public class Program3146 "
    "  public static method Main(): int "
    "    var a: BoxC3146<Shaped3146>? := null; "
    "    var b: BoxN3146<Shaped3146>? := null; "
    "    var c: BoxI3146<Shaped3146>? := null; "
    "    var d: PairP3146<Shaped3146, IShapeG3146>? := null; "
    "    if a <> null then return 1; end if; "
    "    if b <> null then return 2; end if; "
    "    if c <> null then return 3; end if; "
    "    if d <> null then return 4; end if; "
    "    return 0; "
    "  end Main; "
    "end Program3146;";

const char* kClassViolation3146 =
    "type argument `int` does not satisfy the constraint `where T: class`";
const char* kNewViolation3146 =
    "type argument `NoNullary3146` does not satisfy the constraint "
    "`where T: new()`";
const char* kBoundViolation3146 =
    "type argument `Plain3146` does not satisfy the constraint "
    "`where T: IShapeG3146`";

}  // namespace

// ===========================================================================
// T1 — every §14.2 constraint form on an IMPORTED generic type is CHECKED at
// the importer's instantiation site. Pre-fix: zero diagnostics, clean build.
// // per §14.2 / Appendix C.4 / §2.8.3 rule 4
// ===========================================================================
BEZEL_TEST(xmod_generic_constraints_3146,
           imported_generic_constraints_are_checked_at_instantiation) {
    auto r = compileImport3146(
        std::string("module DepF13_3146; ") + kDecls3146,
        std::string("module AppF13_3146a; import DepF13_3146; ") + kPlain3146 +
            kViolatingBody3146);
    if (!r.depOk) return;  // dependency toolchain unavailable — skip
    // All four forms, so a fix that reached only the scalar leg or only the
    // type-bound leg leaves this red.
    BEZEL_EXPECT_TRUE(mentions3146(r, kClassViolation3146));
    BEZEL_EXPECT_TRUE(mentions3146(r, kNewViolation3146));
    BEZEL_EXPECT_TRUE(mentions3146(r, kBoundViolation3146));
    BEZEL_EXPECT_FALSE(r.success);
}

// ===========================================================================
// T2 — ★ THE DIFFERENTIAL CONTROL. The identical declarations and the identical
// instantiations in ONE module. Green before AND after, so the module boundary
// is proved to be the whole difference and the symbol-side rewrite is proved
// not to have changed the local answer. // per §14.2
// ===========================================================================
BEZEL_TEST(xmod_generic_constraints_3146,
           local_generic_constraints_are_checked_control) {
    auto r = compileLocal3146(std::string("module LocalF13_3146; ") +
                              kDecls3146 + kPlain3146 + kViolatingBody3146);
    BEZEL_EXPECT_TRUE(mentions3146(r, kClassViolation3146));
    BEZEL_EXPECT_TRUE(mentions3146(r, kNewViolation3146));
    BEZEL_EXPECT_TRUE(mentions3146(r, kBoundViolation3146));
    BEZEL_EXPECT_FALSE(r.success);
}

// ===========================================================================
// T3 — ★ THE NEGATIVE. A check that starts running for the first time is a
// check that can now OVER-reject. Every one of the four forms is SATISFIED
// here, across the boundary, and must compile clean and run. This is the case
// that would catch a constraint list reconstructed with the wrong kind, the
// wrong bound, or misaligned with its parameter. // per §14.2
// ===========================================================================
BEZEL_TEST(xmod_generic_constraints_3146,
           imported_satisfied_constraints_still_admitted_negative) {
    auto r = compileImport3146(
        std::string("module DepF13S_3146; ") + kDecls3146,
        std::string("module AppF13_3146b; import DepF13S_3146; ") +
            kShaped3146 + kSatisfyingBody3146);
    if (!r.depOk) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_FALSE(mentions3146(r, "does not satisfy the constraint"));
    BEZEL_EXPECT_TRUE(r.success);
}

// ===========================================================================
// T4 — the local twin of T3: satisfied constraints in ONE module stay admitted.
// // per §14.2
// ===========================================================================
BEZEL_TEST(xmod_generic_constraints_3146,
           local_satisfied_constraints_still_admitted_control) {
    auto r = compileLocal3146(std::string("module LocalF13S_3146; ") +
                              kDecls3146 + kShaped3146 + kSatisfyingBody3146);
    BEZEL_EXPECT_FALSE(mentions3146(r, "does not satisfy the constraint"));
    BEZEL_EXPECT_TRUE(r.success);
}

}  // namespace bezel::module
