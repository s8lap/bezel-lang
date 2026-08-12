// Spec-derived regression test for issue #2867 — the CROSS-MODULE half.
//
// §8.5 (`ToString`) / §14.1 (generics are monomorphized) / §31.1 (Throwing) /
// §2.5 (imported artifacts) / §28.9 (Runtime-managed types), v0.240.
//
// The single-module half lives in
// tests/codegen/object_tostring_producer_residual_2867_test.cpp. This file
// exists because #2846 — the fix these two arms are the residual of — had
// exactly this miss: a first draft fixed every single-module shape and left the
// cross-module one leaking, because an imported spelling resolves to a
// different TypeRef shape than the local one. Both of #2867's arms are keyed on
// the SAME predicates `lowerObjectToString`'s call site uses
// (`isBuiltinExceptionTypeRef`, and the `genericSubstSemaTy_` Class/Interface
// test), so producer-recognition and lowering cannot diverge across the module
// boundary by construction — but "by construction" is an inference, and #2846's
// history is why this file measures it instead.
//
// MEASURED with a real two-module build (`bezelc -c lib.bzl` then
// `bezelc --sanitize=address --import lib.o app.bzl`), 20 iterations of each
// shape in one program:
//   pre-fix      1422 bytes leaked in 80 allocations —
//                640 B / 20 objects from `bz$Lib.K$ToString` (the imported
//                class reached through a bare `T`) and 640 B / 20 from
//                `bz$Lib.MyEx$ToString` (the imported exception subclass
//                reached through the built-in `Exception` static type), plus
//                their 71 B / 20 indirect data buffers each
//   with the fix clean, exit 0, same printed total (153)
//
// LOOP-AMPLIFIED with a churn allocation for the reason the codegen half
// documents: a one-shot `Main` is not a reliable LSan oracle, because a
// leftover pointer in a register makes a leaked block read as merely reachable.
// Every rendered string is built at RUNTIME (`"K" + _n.ToString()`), never
// folded from two literals — a literal is an immortal sentinel on which release
// is a no-op, which would make the probe vacuous. Generated-code ASan/LSan is
// enabled on the importer through `CodegenOptions::sanitizeAddress`, and
// LeakSanitizer forces the process exit code to 1 on any leak — so `exit == 0`
// IS the leak assertion here. The program also prints and checks the exact
// rendered total, so a "fix" that stopped producing the strings would not pass.
//
// The exception subclasses here take a LITERAL base message on purpose:
// `base(<a +1 string producer>)` stores the message slot un-retained while the
// shared built-in disposer releases it — a heap-use-after-free filed separately
// as #2902, which would abort this program before it could measure anything.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // #3156: the ONE sanitizer-honouring compileFile wrapper
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2867 {
    std::string path;
    explicit TempFile2867(std::string p) : path(std::move(p)) {}
    ~TempFile2867() { std::remove(path.c_str()); }
};

void writeSource2867(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build or did not exit normally. Mirrors the #2846 sibling helper, with
// generated-code ASan/LSan turned on for the importer (this test's whole point
// is the leak, and LSan reports it by forcing a non-zero exit).
int compileImportAndRunAsan2867(const std::string& libSrc,
                                const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2867_" + std::to_string(std::rand());
    TempFile2867 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2867 aObj((dir / (stem + "_a.o")).string());
    writeSource2867(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2867 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2867 bExe((dir / stem).string());
    writeSource2867(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps, opts).success)
        return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — the IMPORTED spellings of BOTH arms #2867 adds, in one program.
//
//   * `Len<K2867>(k)` — a bare generic-parameter receiver whose active
//     monomorphization binds `T` to an IMPORTED class with a minting
//     `override ToString()` (§14.1: generics are monomorphized). Pre-fix:
//     640 + 71 bytes over 20 iterations from `bz$Lib.K$ToString`.
//   * `e.ToString()` where `e: Exception` holds an IMPORTED user exception
//     subclass with a minting override (§31.1 admits any class as a
//     throwable; §8.5 fixes what its `ToString` produces). Pre-fix:
//     640 + 71 bytes over 20 iterations from `bz$Lib.MyEx$ToString`.
//
// t is the exact sum of the rendered lengths, so a "fix" that changed the
// §8.5 rendering rather than its lifetime would fail here.
// ===========================================================================
BEZEL_TEST(xmod_object_tostring_producer_residual_2867,
           imported_generic_class_and_imported_exception_subclass) {
    const char* lib =
        "module TsLib2867T1; "
        "public class K2867 "
        "  private _n: int; "
        "  public constructor(n: int) "
        "    _n := n; "
        "  end constructor; "
        "  public override method ToString(): string "
        "    return \"K\" + _n.ToString(); "
        "  end ToString; "
        "end K2867; "
        "public class MyEx2867: Exception "
        "  private _n: int; "
        "  public constructor(n: int) base(\"m\"); "
        "    _n := n; "
        "  end constructor; "
        "  public override method ToString(): string "
        "    return \"E\" + _n.ToString(); "
        "  end ToString; "
        "end MyEx2867; ";
    int r = compileImportAndRunAsan2867(
        lib,
        "module App2867T1; import TsLib2867T1; "
        "public class C2867T1 "
        "  public static method Len<T>(v: T): int where T: class "
        "    var s: string := v.ToString(); "
        "    return s.Length; "
        "  end Len; "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      using var k := new K2867(i); "
        "      t := t + Len<K2867>(k); "
        "      using var e: Exception := new MyEx2867(i); "
        "      var s: string := e.ToString(); "
        "      var churn: string := \"c\" + i.ToString(); "
        "      t := t + s.Length + churn.Length; "
        "    end while; "
        "    if t <> 153 then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end C2867T1;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T2 — the IMPORTED SENTINEL controls: an imported exception subclass with NO
// `ToString` override, and an imported class with none, both reached through
// the two arms this fix widens. §8.5: "The compiler-generated default returns:
// - For classes: the type's qualified name" — an immortal literal, so neither
// ever leaked and neither must now OVER-release. `bz_string_release` returns
// early on the sentinel refcount, so a bare release on one is a no-op by
// construction; this pins that the widening did not disturb them, and pins the
// rendered TEXT so the fix is proven to change lifetime only.
// ===========================================================================
BEZEL_TEST(xmod_object_tostring_producer_residual_2867,
           imported_sentinel_defaults_are_not_over_released) {
    const char* lib =
        "module TsLib2867T2; "
        "public class PlainEx2867: Exception "
        "  public constructor() base(\"m\"); end constructor; "
        "end PlainEx2867; "
        "public class PlainK2867 "
        "  public constructor() end constructor; "
        "end PlainK2867; ";
    int r = compileImportAndRunAsan2867(
        lib,
        "module App2867T2; import TsLib2867T2; "
        "public class C2867T2 "
        "  public static method Nm<T>(v: T): string where T: class "
        "    return v.ToString(); "
        "  end Nm; "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    var acc: string := \"\"; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      using var pe: Exception := new PlainEx2867(); "
        "      using var pk := new PlainK2867(); "
        "      acc := pe.ToString() + \"|\" + Nm<PlainK2867>(pk); "
        "      var churn: string := \"c\" + i.ToString(); "
        "      if churn.Length < 1 then return 4; end if; "
        "    end while; "
        // #2934 (§8.5, v0.252): both sentinel defaults are now the
        // MODULE-PATH-QUALIFIED name, and an imported type carries its
        // EXPORTER's module path (#1488's TypeSymbol::moduleName, which
        // ModuleLoader restores from the dependency's metadata). Still a
        // pooled immortal literal, so the lifetime property this test
        // exists to pin is unchanged — only the text.
        "    if acc <> \"TsLib2867T2.PlainEx2867|TsLib2867T2.PlainK2867\" then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end C2867T2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

}  // namespace bezel::module
