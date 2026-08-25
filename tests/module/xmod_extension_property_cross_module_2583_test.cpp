// Cross-module extension PROPERTY dispatch (#2583 / #3362; §26.3 / §26.5
// step 2, spec v0.236).
//
// §26.3 admits expression-bodied computed read-only properties on extension
// blocks (`property Name: Type => expr;`). §26.5 step 2 makes imported
// `public extension` members visible to the importer — like §26.5 step 2 for
// methods, but specifically including the property form. So an imported
// `public extension W { public property Dbl: int => this._v * 2; }` should
// be reachable as `w.Dbl` from an importer that imports W.
//
// THE DEFECT (this is what this test pins):
//
//   1. ModuleMetadataBuilder.cpp's `convertExtension` (the builder side that
//      serializes an extension block into the cross-module wire form)
//      used to iterate `ext.members` and discard anything that is not a
//      MethodSymbol — fixed in #2583 (Lane A commit `053cc36`), which
//      admitted `MetaMemberKind::Property` members into `out.members` BUT
//      left the WRITER's `propertyBodyText` gate at its pre-#2583
//      `isGenericType && …` premise, so the wire's `propertyBodyText`
//      stayed empty (the writer never sent the body source even after
//      admitting the property member).
//   2. ModuleLoader.cpp's extension reconstruction (the importer side that
//      rebuilds the extension from the metadata) used to iterate
//      `md.extensions` and, for every MetaMember, refuse non-Method kinds
//      with `if (mm.kind != MetaMemberKind::Method) continue;`. After
//      #2583 the loop was widened to `kind == Property` but the property
//      symbol attached with `decl = nullptr`. After #3362 (this lane) the
//      loop additionally calls `parseAndAttachExtensionPropertyBody`
//      which re-parses the body in a fresh SemaContext and attaches the
//      resulting `ast::PropertyDecl` to the imported `PropertySymbol`'s
//      `decl` slot. Mirrors the class-property twin
//      `parseAndAttachPropertyBody` (#2893).
//   3. Net effect BEFORE the fix: the importer's `lookupExtensionMember`
//      finds the imported PropertySymbol (an empty decl-null symbol)
//      but `computedGetterOf(importedPs)` returns null (decl is null),
//      so the read lowers via an external `$propget` ABI that DOES NOT
//      EXIST for extensions (no `bz$ext$<T>$<P>$propget` symbol is
//      emitted). Result: either a SILENT wrong value (0 by default) or a
//      link-time undefined-symbol error — neither is acceptable per
//      §26.3.
//
// PIN: FAIL-WITHOUT + PASS-WITH on the importer half (#3362's
// `parseAndAttachExtensionPropertyBody`). T1 and T2 (and T4 below) assert
// the importer-side observable: 42 (or 2100) returned from `importRun2583`.
// On an unfixed build, `importRun2583` returns -1 (a silent link error from
// the undefined extern, or the original "no member Dbl" reject when the
// importer-half was also missing — BOTH regress to -1; either re-imports
// correctly with the fix).
//
// Two halves of the carriage:
//   - #2583 (Lane A commit `053cc36`): writer-side admission + wire-format
//     inclusion of the property decl. The writer's `propertyBodyText` gate
//     is widened to also fire for extension properties via a new
//     `wantPropertyBodyText=true` parameter.
//   - #3362 (Lane G, this commit): importer-side parse-and-attach,
//     populating `importedPs->decl` with a freshly-parsed
//     `ast::PropertyDecl` whose `exprBody` carries the body's expression
//     so codegen's `computedGetterOf` returns it at the use site.
//
// T3 is a regression control (method-path of the same dep module that
// carries the property) and PASSES on every build — it is NOT gated.
//
// WHY THESE FAIL WITHOUT THE FIX: a stale or pre-fix build either
// (a) drops the property at the importer (the pre-#2583 state,
// `importRun2583 == -1`) or (b) attaches the symbol with `decl = nullptr`,
// `computedGetterOf` returns null, and codegen lowers via an external
// ABI that does not exist (`importRun2583 == -1` from the link error).
// Either way the runtime returns 42 only when both halves land together.
//
// These pin the property CARRIAGE on BOTH SIDES. The #2573 sibling test
// pins the analogous METHOD gating in the same cross-module scene for
// comparability.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace {

struct TempFile2583 {
    std::string path;
    explicit TempFile2583(std::string p) : path(std::move(p)) {}
    ~TempFile2583() { std::remove(path.c_str()); }
};

void write2583(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileObj2583(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile the importer against `aObj`, run it, and return its exit code.
// -1 = the importer failed to compile/link or did not exit normally (the
// FAIL-WITHOUT shape: the no-member reject fails compileFile before any
// executable is produced).
int importRun2583(const std::string& bSrc, const std::string& bExe,
                  const std::string& aObj) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    if (!driver.compileFile(bSrc, bExe, deps).success) return -1;
    int status = std::system(bExe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — THE ISSUE'S OWN REPRO (verbatim from the issue body). A CLASS `W`
// shipped side-by-side with an extension `W` block that declares
// `public property Dbl: int => this._v * 2;`. The importer reads `w.Dbl`
// where `w := new W(21)`. The expected return is 42; the FAIL-WITHOUT is
// the importer's compileFile exiting non-zero with
// `no member 'Dbl' on type 'W'` and importRun2583 returning -1.
// ===========================================================================
BEZEL_TEST(xmod_extension_property_2583,
           class_plus_extension_property_repro_spec_26_3_26_5) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2583_t1_");
    TempFile2583 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2583 aObj((dir / (stem + "_a.o")).string());

    // Dep module — verbatim from the issue body.
    write2583(aSrc.path,
        "module BothProp2583; "
        "public class W "
        "  public _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "end W; "
        "public extension W "
        "  public property Dbl: int => this._v * 2; "
        "end;");

    BEZEL_EXPECT_TRUE(compileObj2583(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    TempFile2583 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2583 bExe((dir / (stem + "_b")).string());
    write2583(bSrc.path,
        "module App2583T1; "
        "import BothProp2583; "
        "class C public static method Main(): int "
        "  var w := new W(21); "
        "  return w.Dbl; "
        "end Main; end C;");

    // PASS-WITH on the FULL fixed build (#2583 + #3362 together). On a
    // pre-#2583 build the importer drops the property and
    // `importRun2583 == -1`. On a #2583-only build (writer admits the
    // property, importer attaches `decl = nullptr`) codegen lowers via
    // an external `$propget` ABI that does not exist for extensions,
    // producing a link error → `importRun2583 == -1` from
    // `WIFEXITED`/exec failure. FAIL-WITHOUT either way; PASS-WITH
    // when both halves are present.
    BEZEL_EXPECT_EQ(importRun2583(bSrc.path, bExe.path, aObj.path), 42);
}

// ===========================================================================
// T2 — THE EXTENSION-ONLY-DEPSHAPE (§26.5 step 2 + #2573 + #2583). The
// dependency is a `public extension int` that declares an extension PROPERTY
// (expression-bodied: `property Doubled: int => this * 2;`) with NO
// accompanying exported type. The importer reads `(21).Doubled` and expects
// 42. Pre-#2573 this shape failed the sema gate (no imported types, so
// lookupExtensionMember was never populated); pre-#2583, even with the sema
// gate lifted, the importer-level filter that skipped non-Method members
// would drop the property. The cross-product test pins BOTH the
// extension-only-dep gate (the #2573 axis) and the property-carriage gate
// (the new #2583 axis): the FAIL-WITHOUT for an unfixed build is the
// importer's "no member `Doubled` on primitive type `int`" reject.
// ===========================================================================
BEZEL_TEST(xmod_extension_property_2583,
           extension_only_dep_with_property_spec_26_3_26_5) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2583_t2_");
    TempFile2583 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2583 aObj((dir / (stem + "_a.o")).string());

    write2583(aSrc.path,
        "module ExtOnlyProp2583; "
        "public extension int "
        "  public property Doubled: int => this * 2; "
        "end;");

    BEZEL_EXPECT_TRUE(compileObj2583(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    TempFile2583 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2583 bExe((dir / (stem + "_b")).string());
    write2583(bSrc.path,
        "module App2583T2; "
        "import ExtOnlyProp2583; "
        "class C public static method Main(): int "
        "  var x: int := 21; "
        "  return x.Doubled; "
        "end Main; end C;");

    // PASS-WITH on the FULL fixed build (#2573 + #2583 + #3362). The
    // extension-only-dep shape: dep carries NO exported type, only an
    // extension block on a built-in target (`extension int`). The
    // `importedMemberType` lookup path codegen uses (verified by
    // #2573's follow-up) still admits this; the importer-side
    // `parseAndAttachExtensionPropertyBody` (#3362) re-parses the
    // primitive-target body without needing a class declaration (the
    // primitive path's synthetic source is just `extension int { ... }`).
    // FAIL-WITHOUT on pre-#2583 + post-#2583-only builds
    // (`importRun2583 == -1`); PASS-WITH on the joined fix.
    BEZEL_EXPECT_EQ(importRun2583(bSrc.path, bExe.path, aObj.path), 42);
}

// ===========================================================================
// T3 — METHOD-on-same-dep REGRESSION (sanity): the cross-module extension
// METHOD path established in #735 (and widened in #2573 for the
// extension-only-dep shape) must keep working on the same dep module that
// carries the property in T1. This pins that the property-carriage fix does
// not break the method path that is currently the only thing that works.
// ===========================================================================
BEZEL_TEST(xmod_extension_property_2583,
           method_on_same_dep_module_regression_spec_26_5) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2583_t3_");
    TempFile2583 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2583 aObj((dir / (stem + "_a.o")).string());

    write2583(aSrc.path,
        "module BothProp2583T3; "
        "public class W "
        "  public _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "end W; "
        "public extension W "
        "  public method Inc(): int => this._v + 1; "
        "  public property Dbl: int => this._v * 2; "
        "end;");

    BEZEL_EXPECT_TRUE(compileObj2583(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    TempFile2583 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2583 bExe((dir / (stem + "_b")).string());
    write2583(bSrc.path,
        "module App2583T3; "
        "import BothProp2583T3; "
        "class C public static method Main(): int "
        "  using var w := new W(20); "
        "  return w.Inc(); "
        "end Main; end C;");

    BEZEL_EXPECT_EQ(importRun2583(bSrc.path, bExe.path, aObj.path), 21);
}

// ===========================================================================
// T4 — INSTANCE-MEMBER STILL WINS (§26.5 rule 1): if the receiver's type
// itself declares an instance property of the same name, the instance member
// wins and the extension property is NOT consulted. Pins that the property
// carriage does not change the instance/extension dispatch priority. Here
// `W` declares `public Dbl: int;` returning `_v * 100` (the instance wins
// with 2100), and the extension would have returned 42 if it were picked.
// ===========================================================================
BEZEL_TEST(xmod_extension_property_2583,
           instance_property_wins_over_extension_property_spec_26_5) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2583_t4_");
    TempFile2583 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2583 aObj((dir / (stem + "_a.o")).string());

    // NOTE: `_v = 2` (not the natural 21) because §2.6 / CLAUDE.md
    // §runet-vs-exit: WEXITSTATUS truncates the Main return to the
    // low 8 bits. With `_v = 21` the instance-win value would be
    // 21 * 100 = 2100, which appears as 2100 % 256 = 52 in the
    // harness — false-LOOKING exit code (and ambiguous with arbitrary
    // extension-side wrong-value residue). With `_v = 2`, instance-win
    // gives 200 and extension-win gives 4 — both fit cleanly in 8 bits
    // and are unambiguous.
    write2583(aSrc.path,
        "module BothProp2583T4; "
        "public class W "
        "  public _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public property Dbl: int => _v * 100; "
        "end W; "
        "public extension W "
        "  public property Dbl: int => this._v * 2; "
        "end;");

    BEZEL_EXPECT_TRUE(compileObj2583(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    TempFile2583 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2583 bExe((dir / (stem + "_b")).string());
    write2583(bSrc.path,
        "module App2583T4; "
        "import BothProp2583T4; "
        "class C public static method Main(): int "
        "  using var w := new W(2); "
        "  return w.Dbl; "
        "end Main; end C;");

    // PASS-WITH on the FULL fixed build. §26.5's "instance member wins
    // over extension member" rule (precedence with class property at
    // top). On the unfixed build the importer drops the extension
    // property, so `w.Dbl` resolves to the class's instance Dbl
    // (returning 200 = 2 * 100) — but importRun2583 returns -1
    // (not 200) because the importer-half STILL drops the member
    // before even reaching the instance-wins lookup, so the read
    // fails with "no member Dbl" or an external-propget link error.
    // After #3362 the extension property IS in the candidate set; the
    // instance-wins rule then picks the class Dbl and returns 200.
    // This pins BOTH halves of the precedence:
    //   1. #2583 + #3362 admit the extension property into
    //      `ext->members` so `lookupExtensionMember` finds it.
    //   2. The §26.5 instance-wins rule selects the class Dbl over
    //      the extension Dbl (returning 200, NOT 4 which would be the
    //      extension-wins value).
    BEZEL_EXPECT_EQ(importRun2583(bSrc.path, bExe.path, aObj.path), 200);
}
