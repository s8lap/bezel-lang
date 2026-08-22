// Regression test for issue #3275 (§23 / §29.1+§29.2, v0.254): pin the imported-
// symbol invariant that `PropertySymbol::decl` is null for every IMPORTED
// PropertySymbol.
//
// The investigation (Lane D Wave 1) verified empirically on HEAD `7c600fe`
// that `currentPropSym->decl` is null for every property-bearing shape the
// for-in driver / wrapper can land on:
//   (T1) method-form `Current(): T`                 → no `currentPropSym`
//                                                      set; `currentMethodSym`
//                                                      holds; no `decl` read.
//   (T2) non-virtual class-parented `Current`        → `importedPropGetterFns_`
//                                                      populated, the `else`
//                                                      arm reads `currentFn`
//                                                      from it (no thunk
//                                                      build, no `pd`).
//   (T3) virtual class-parented `Current`           → same path post-#3267
//                                                      (`declareImportedPropertyGetters`
//                                                      populates the thunk for
//                                                      VIRTUAL imported
//                                                      properties — fix
//                                                      `251213d`).
//   (T4) interface-parented `Current`                → `importedPropGetterFns_`
//                                                      skip path; the `else`
//                                                      arm reads nothing.
//
// The refute commit `1a74041`'s `!ctx_.isImported(enumTs)` band-aid at
// `wrapUserClassAsIEnumerable` / `lowerIEnumerableFor` was preserved as
// defensive until #3275 investigated — it was REVERTED in the structural
// fix #3267 (`251213d`). The current pin is:
//   * the `#ifndef NDEBUG` invariant check at both call sites
//     (`LlvmBackend_Assign.cpp` ~L9782, `LlvmBackend_Stmts.cpp` ~L7985) that
//     fires on any future loader change that re-introduces `decl` on an
//     imported PropertySymbol (debug builds only; release codegen unchanged);
//   * this regression test, which exercises all four shapes and asserts
//     cross-module == twin (mechanical, twin-driven rather than hand-written
//     6 — survives spec revisions that change element-type numerics or the
//     `for` syntax, since the agreement is what is mechanically pinned).
//
// Spec authority: §23 (property accessors), §29.1 + §29.2 + §29.11
// (for-in / for-over-iterable protocol), §2.5 / §2.8.2 / §2.8.3 (module
// boundary, cross-module ABI); #3275 follow-up to refute #3267
// (`1a74041`); structural fix #3267 (`251213d`).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile3275 {
    std::string path;
    explicit TempFile3275(std::string p) : path(std::move(p)) {}
    ~TempFile3275() { std::remove(path.c_str()); }
};

void writeSource3275(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib → object, compile app (importing lib) → exe, run it in a
// separate process, return its exit code. -1 on compile/link/run failure,
// -2 if the dependency itself could not be produced (toolchain unavailable).
int compileImportAndRun3275(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3275_");
    TempFile3275 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3275 aObj((dir / (stem + "_a.o")).string());
    writeSource3275(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good())
            return -2;
    }
    TempFile3275 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3275 bExe((dir / stem).string());
    writeSource3275(bSrc.path, appSrc);
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

// The single-module twin runner: same declarations with the module boundary
// removed.
int compileLocalAndRun3275(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3275l_");
    TempFile3275 aSrc((dir / (stem + ".bzl")).string());
    TempFile3275 aExe((dir / stem).string());
    writeSource3275(aSrc.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, aSrc.path, aExe.path).success)
        return -1;
    int status = std::system(aExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// ===========================================================================
// T1 — method-form `Current(): int`.
// The for-in driver resolves the `Current` read through `methodFns_` lookup
// (`currentMethodSym`, not `currentPropSym`); `currentPropSym` is never set,
// so the `decl`-null invariant is trivially satisfied. This shape is the
// direct opposite of the property-bearing shapes (T2/T3) the issue names.
// ===========================================================================
BEZEL_TEST(xmod_imported_property_decl_invariant_3275,
           forin_over_imported_method_current_decl_null_23_29_11) {
    const std::string libSrc =
        "module Lib3275T1; "
        "public class IntEnum3275T1: IEnumerator<int> "
        "  private _v: int := 0; "
        "  public method MoveNext(): bool "
        "    _v := _v + 1; "
        "    return _v <= 3; "
        "  end MoveNext; "
        "  public method Current(): int "
        "    return _v; "
        "  end Current; "
        "end IntEnum3275T1; "
        "public class IntRange3275T1: IEnumerable<int> "
        "  public method GetEnumerator(): owned IEnumerator<int> "
        "    return new IntEnum3275T1(); "
        "  end GetEnumerator; "
        "end IntRange3275T1; ";
    const std::string appSrc =
        "module App3275T1; import Lib3275T1; "
        "public class C "
        "  public static method Main(): int "
        "    using var e := new IntRange3275T1(); "
        "    var s := 0; "
        "    for var x in e do s := s + x; end; "
        "    return s; "
        "  end Main; "
        "end C; ";
    // Strip the leading `module`/`import` preamble from each so the only
    // module declaration in the twin is the wrapper.
    const std::size_t libBodyStart = libSrc.find("public class");
    const std::size_t appBodyStart = appSrc.find("public class");
    const std::string twinSrc =
        "module Single3275T1; " +
        (libBodyStart != std::string::npos
            ? libSrc.substr(libBodyStart)
            : std::string{}) +
        (appBodyStart != std::string::npos
            ? appSrc.substr(appBodyStart)
            : std::string{});
    int twin = compileLocalAndRun3275(twinSrc);
    BEZEL_EXPECT_TRUE(twin >= 0);
    int imported = compileImportAndRun3275(libSrc, appSrc);
    if (imported == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(imported >= 0);
    BEZEL_EXPECT_EQ(imported, twin);
}

// ===========================================================================
// T2 — non-virtual class-parented `Current` property. The exact shape the
// fix-#3197 comment at `wrapUserClassAsIEnumerable` and `lowerIEnumerableFor`
// names: "`Current` is non-virtual and class-parented, so neither of
// `declareImportedPropertyGetters`' two skips (virtual, interface-parented)
// applies". For this shape, `importedPropGetterFns_[currentPropSym]` is
// populated, the `else` arm reads `currentFn` from it, and the `pd` arm
// is unreachable. Empirical verification on HEAD `7c600fe` (debug prints
// added at both sites for the Lane D investigation) confirmed
// `currentPropSym->decl == nullptr` here. The pin: if the loader ever
// regresses (re-introduces `decl` on the imported PropertySymbol), the
// `#ifndef NDEBUG` invariant check at `wrapUserClassAsIEnumerable` fires
// and the build aborts at the first compile of this test.
// ===========================================================================
BEZEL_TEST(xmod_imported_property_decl_invariant_3275,
           forin_over_imported_nonvirtual_current_decl_null_23_29_11) {
    const std::string libSrc =
        "module Lib3275T2; "
        "public class IntEnum3275T2: IEnumerator<int> "
        "  private _v: int := 0; "
        "  public method MoveNext(): bool "
        "    _v := _v + 1; "
        "    return _v <= 3; "
        "  end MoveNext; "
        "  public property Current: int get => _v; end Current; "
        "end IntEnum3275T2; "
        "public class IntRange3275T2: IEnumerable<int> "
        "  public method GetEnumerator(): owned IEnumerator<int> "
        "    return new IntEnum3275T2(); "
        "  end GetEnumerator; "
        "end IntRange3275T2; ";
    const std::string appSrc =
        "module App3275T2; import Lib3275T2; "
        "public class C "
        "  public static method Main(): int "
        "    using var e := new IntRange3275T2(); "
        "    var s := 0; "
        "    for var x in e do s := s + x; end; "
        "    return s; "
        "  end Main; "
        "end C; ";
    const std::size_t libBodyStart = libSrc.find("public class");
    const std::size_t appBodyStart = appSrc.find("public class");
    const std::string twinSrc =
        "module Single3275T2; " +
        (libBodyStart != std::string::npos
            ? libSrc.substr(libBodyStart)
            : std::string{}) +
        (appBodyStart != std::string::npos
            ? appSrc.substr(appBodyStart)
            : std::string{});
    int twin = compileLocalAndRun3275(twinSrc);
    BEZEL_EXPECT_TRUE(twin >= 0);
    int imported = compileImportAndRun3275(libSrc, appSrc);
    if (imported == -2) return;
    BEZEL_EXPECT_TRUE(imported >= 0);
    BEZEL_EXPECT_EQ(imported, twin);
}

// ===========================================================================
// T3 — VIRTUAL class-parented `Current` property. The exact shape fix #3267
// (`251213d`) is named for. Pre-#3267 cross-module returned 0; post-#3267
// cross-module == twin. The fix populated `importedPropGetterFns_` for
// VIRTUAL imported properties with the `importedVirtualAccessorSlotFn`
// thunk (#3190), which loads the function pointer from the OWNER's
// `vt$<Owner>[slot]` and forwards `this` to it. For this shape too,
// `currentPropSym->decl` is null on HEAD — the loader never sets `decl` on
// any imported PropertySymbol, regardless of virtuality. The pin: same
// invariant check + same twin-oracle assertion.
// ===========================================================================
BEZEL_TEST(xmod_imported_property_decl_invariant_3275,
           forin_over_imported_virtual_current_decl_null_23_29_11) {
    const std::string libSrc =
        "module Lib3275T3; "
        "public class IntEnum3275T3: IEnumerator<int> "
        "  private _v: int := 0; "
        "  public method MoveNext(): bool "
        "    _v := _v + 1; "
        "    return _v <= 3; "
        "  end MoveNext; "
        "  public virtual property Current: int get => _v; end Current; "
        "end IntEnum3275T3; "
        "public class IntRange3275T3: IEnumerable<int> "
        "  public method GetEnumerator(): owned IEnumerator<int> "
        "    return new IntEnum3275T3(); "
        "  end GetEnumerator; "
        "end IntRange3275T3; ";
    const std::string appSrc =
        "module App3275T3; import Lib3275T3; "
        "public class C "
        "  public static method Main(): int "
        "    using var e := new IntRange3275T3(); "
        "    var s := 0; "
        "    for var x in e do s := s + x; end; "
        "    return s; "
        "  end Main; "
        "end C; ";
    const std::size_t libBodyStart = libSrc.find("public class");
    const std::size_t appBodyStart = appSrc.find("public class");
    const std::string twinSrc =
        "module Single3275T3; " +
        (libBodyStart != std::string::npos
            ? libSrc.substr(libBodyStart)
            : std::string{}) +
        (appBodyStart != std::string::npos
            ? appSrc.substr(appBodyStart)
            : std::string{});
    int twin = compileLocalAndRun3275(twinSrc);
    BEZEL_EXPECT_TRUE(twin >= 0);
    int imported = compileImportAndRun3275(libSrc, appSrc);
    if (imported == -2) return;
    BEZEL_EXPECT_TRUE(imported >= 0);
    BEZEL_EXPECT_EQ(imported, twin);
}

// ===========================================================================
// T4 — interface-parented `Current` property. The shape that
// `declareImportedPropertyGetters`' second skip targets ("skip if
// interface-parented"): `importedPropGetterFns_` has no entry for this
// property, the `else` arm finds nothing, and the wrapper falls through
// (`currentFn == nullptr` → `return src;`). For a for-in driver over an
// enumerable whose enumerator's `Current` is interface-parented, the
// `wrapUserClassAsIEnumerable` path falls through and the driver reaches
// the inner read through the same code as T2/T3. The test pins the
// observable: cross-module == twin, with the for-in returning the sum.
// ===========================================================================
BEZEL_TEST(xmod_imported_property_decl_invariant_3275,
           forin_over_imported_iface_current_decl_null_23_29_11) {
    const std::string libSrc =
        "module Lib3275T4; "
        // The interface-parented enumerator: `Current` is declared on an
        // interface that the concrete class implements. `declareImportedPropertyGetters`
        // skips interface-parented properties, so `importedPropGetterFns_`
        // has no entry — but `currentPropSym->decl` is still null (the
        // loader invariant is unconditional). The interface property is
        // declared read-only (`readonly` modifier) so the concrete
        // implementation's getter-only declaration is acceptable per
        // §11.2 / §23.3 (a getter-only implementation of a read-write
        // interface property is rejected).
        "public interface ICurrent3275T4 "
        "  readonly property Current: int; "
        "end ICurrent3275T4; "
        "public class IntEnum3275T4: IEnumerator<int>, ICurrent3275T4 "
        "  private _v: int := 0; "
        "  public method MoveNext(): bool "
        "    _v := _v + 1; "
        "    return _v <= 3; "
        "  end MoveNext; "
        "  public property Current: int get => _v; end Current; "
        "end IntEnum3275T4; "
        "public class IntRange3275T4: IEnumerable<int> "
        "  public method GetEnumerator(): owned IEnumerator<int> "
        "    return new IntEnum3275T4(); "
        "  end GetEnumerator; "
        "end IntRange3275T4; ";
    const std::string appSrc =
        "module App3275T4; import Lib3275T4; "
        "public class C "
        "  public static method Main(): int "
        "    using var e := new IntRange3275T4(); "
        "    var s := 0; "
        "    for var x in e do s := s + x; end; "
        "    return s; "
        "  end Main; "
        "end C; ";
    const std::size_t libBodyStart = libSrc.find("public interface");
    const std::size_t appBodyStart = appSrc.find("public class");
    const std::string twinSrc =
        "module Single3275T4; " +
        (libBodyStart != std::string::npos
            ? libSrc.substr(libBodyStart)
            : std::string{}) +
        (appBodyStart != std::string::npos
            ? appSrc.substr(appBodyStart)
            : std::string{});
    int twin = compileLocalAndRun3275(twinSrc);
    BEZEL_EXPECT_TRUE(twin >= 0);
    int imported = compileImportAndRun3275(libSrc, appSrc);
    if (imported == -2) return;
    BEZEL_EXPECT_TRUE(imported >= 0);
    BEZEL_EXPECT_EQ(imported, twin);
}

}  // namespace
}  // namespace bezel::module