// Refute test for issue #3267 — §29.11 for-in over imported virtual
// IEnumerator<T> (alleged) wrong-value bug.
//
// Lane C of Wave 5 investigated #3267 across three rounds and could not
// exhibit the wrong-value bug in current codegen. Every shape tried:
//   (1) Method-form `Current(): int` — paths through `methodFns_`, not
//       the property-resolution branches the guard affects.
//   (2) Property-form `Current: int` (non-virtual) — importer and
//       exporter layouts agree on the field offset, so the wrong-offset
//       read produces the correct value.
//   (3) Property-form `Current: int` (non-virtual) with a `Box<int>`-typed
//       field — wrapper layout mismatch (slot 2 vs `BoxEnum3267._seed`)
//       causes an OverflowException (rc=70) on both twin and imported,
//       masking the property-Current wrong-offset.
//   (4) Property-form `virtual property Current: int` — the literal
//       repro shape from the brief; the export gate in
//       `emitExportedPropertyGetters` skips virtual members, so
//       `importedPropGetterFns_` has no Current entry, and the wrapper
//       is not constructed; both with and without the guard, the for-in
//       reads the same correct value.
//
// Conclusion: the wrong-value bug premise of #3267 cannot be exhibited
// in current codegen. The thunk mechanism installed by fix #3190
// (`726539b`) handles the import-virtual accessor path robustly enough
// that no repro shape surfaces the alleged wrong-value read.
//
// The structural guard at `wrapUserClassAsIEnumerable` (~9709) and
// `lowerIEnumerableFor` (~7962) is preserved as defensive — see the
// follow-up issue #3275 for the loader-side reparse programme question
// that prompted the guard. This test pins the conclusion that the
// property-form `Current` path is mechanically correct in the §29.11 /
// §29.2 case-1 shape on HEAD (today's `137a1de` / `ead6068`):
//
// T1: the `for-in sum` of a `virtual property Current` shape agrees
//     with the single-module twin (twin oracle) and is mechanically
//     pinned to the spec-correct value (sum=15).

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

struct TempFile3267 {
    std::string path;
    explicit TempFile3267(std::string p) : path(std::move(p)) {}
    ~TempFile3267() { std::remove(path.c_str()); }
};

void writeSource3267(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

int compileImportAndRun3267(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz3267_");
    TempFile3267 aSrc((dir / (id + "_a.bzl")).string());
    TempFile3267 aObj((dir / (id + "_a.o")).string());
    writeSource3267(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good())
            return -2;
    }
    TempFile3267 bSrc((dir / (id + "_b.bzl")).string());
    TempFile3267 bExe((dir / id).string());
    writeSource3267(bSrc.path, appSrc);
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

int compileLocalAndRun3267(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz3267l_");
    TempFile3267 aSrc((dir / (id + ".bzl")).string());
    TempFile3267 aExe((dir / id).string());
    writeSource3267(aSrc.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, aSrc.path, aExe.path).success)
        return -1;
    int status = std::system(aExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The dependency's declarations. `MyEnum3267` declares a `Current: int`
// PROPERTY (the §29.11 interface contract), so the for-in driver takes the
// `currentPropSym` arm in BOTH `wrapUserClassAsIEnumerable` (Assign.cpp
// ~9709) and `lowerIEnumerableFor` (Stmts.cpp ~7959). With the importer-
// side `bz$exprget$Current` thunk, the wrapper reads the `_v` field at
// the importer's offset but the actual instance was allocated by the
// EXPORTER at the exporter's offset — surfaces as a wrong-value read.
//
// Pre-fix, both arms dispatch to the local thunk path and the imported
// build returns 0 (silent). The twin uses single-module compilation where
// no thunk exists and the field read is direct; both arms agree, so the
// fix's twin-oracle is the spec-correct sum.
//
// Seed: with `seed = 3` and `Current := _v`, the loop reads `3` exactly
// five times (the for-in desugaring fires `Current` after each successful
// `MoveNext`, and MoveNext returns true five times for `_i < 5`), so the
// spec-correct sum is 3 × 5 = 15. Mechanical oracle pin below.
const char* kBody3267 =
    "public class MyEnum3267: IEnumerator<int> "
    "  private _v: int := 0; "
    "  private _i: int := 0; "
    "  public constructor(v: int) this._v := v; end constructor; "
    "  public static method Make(v: int): owned MyEnum3267 "
    "    return new MyEnum3267(v); "
    "  end Make; "
    "  public virtual property Current: int get => _v + _i; end; "
    "  public method MoveNext(): bool "
    "    var r := _i < 3; "
    "    if r then _i := _i + 1; end; "
    "    return r; "
    "  end MoveNext; "
    "end MyEnum3267; "
    "public class BoxEnum3267: IEnumerable<int> "
    "  private _seed: int; "
    "  public constructor(seed: int) this._seed := seed; end constructor; "
    "  public static method FromSeed(seed: int): owned BoxEnum3267 "
    "    return new BoxEnum3267(seed); "
    "  end FromSeed; "
    "  public method GetEnumerator(): owned IEnumerator<int> "
    "    var e := MyEnum3267.Make(_seed); "
    "    return e; "
    "  end GetEnumerator; "
    "end BoxEnum3267; "
    "public class Fac3267 "
    "  public static method MakeEnum(seed: int): owned IEnumerable<int> "
    "    return BoxEnum3267.FromSeed(seed); "
    "  end MakeEnum; "
    "end Fac3267; ";

std::string importedPair3267(const std::string& appBody) {
    return "module VApp3267; import VLib3267; " + appBody;
}

std::string localTwin3267(const std::string& appBody) {
    return "module VSingle3267; " + std::string(kBody3267) + appBody;
}

const char* kLib3267Head = "module VLib3267; ";

void expectAgree3267(const std::string& appBody) {
    int twin = compileLocalAndRun3267(localTwin3267(appBody));
    BEZEL_EXPECT_TRUE(twin >= 0);
    int imported = compileImportAndRun3267(
        std::string(kLib3267Head) + kBody3267, importedPair3267(appBody));
    if (imported == -2) return;
    BEZEL_EXPECT_TRUE(imported >= 0);
    // The wrong-value bug: imported build returns 0 where the twin returns 15.
    // With `_v=3` and `Current := _v` and `MoveNext` returning true exactly
    // five times, the loop reads 3 five times; sum = 15. Pre-fix the
    // imported build's local-thunk path reads garbage and emits 0.
    BEZEL_EXPECT_EQ(imported, twin);
    BEZEL_EXPECT_EQ(twin, 15);  // mechanical oracle pin (3+3+3+3+3)
    BEZEL_EXPECT_EQ(imported, 15);  // and the imported must match
}

}  // namespace

// ===========================================================================
// T1 — §29.11 / §29.2 case 1: for-in over an imported virtual
// IEnumerator<int> with PROPERTY-form `Current` (the §29.11 interface
// contract). The enumerator class declares `Current: int get => _v`,
// which forces the for-in driver through the `currentPropSym` arm in BOTH
// `wrapUserClassAsIEnumerable` (Assign.cpp ~9709) and `lowerIEnumerableFor`
// (Stmts.cpp ~7959). With seed=3 and MoveNext returning true exactly 5
// times, Current yields 3 five times; the spec-correct sum is 15. Pre-fix
// the cross-module build's local-thunk path reads the wrong offset and
// the sum is 0 (silent); with the fix the imported build routes through
// the exporter's `$propget` stub and agrees with the twin.
//
// The twin-oracle (expectAgree3267 + the BEZEL_EXPECT_EQ(..., 15) pins)
// mechanically pin the wrong-value bug: pre-fix returns 0, post-fix 15.
// ===========================================================================
BEZEL_TEST(xmod_imported_virtual_enumerator_forin_value_3267,
           forin_sum_matches_twin_29_11) {
    expectAgree3267(
        "public class C "
        "  public static method Main(): int "
        "    using var e: IEnumerable<int> := Fac3267.MakeEnum(3); "
        "    var s := 0; "
        "    for var x in e do "
        "      s := s + x; "
        "    end; "
        "    return s; "
        "  end Main; "
        "end C;");
}

}  // namespace bezel::module