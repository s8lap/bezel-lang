// Spec-derived regression test for issue #2951 — the CROSS-MODULE half.
//
// §28.4 (The `destructor`, step 3) / §28.2 (exactly-once) / §28.9
// (Runtime-managed types) / §31.4 (Standard exception types) / §2.5 (Modules
// and imports) / §2.8.3 rule 4, v0.240.
//
// The single-module half lives in
// tests/codegen/exception_subclass_header_release_2951_test.cpp. This file
// exists because the cross-module direction is exactly where #2951 was BLOCKED,
// not merely untested: the release, added on a base without #2950, made the
// EXPORTER-compiled disposer hand the IMPORTER's field value to
// `bz_string_release` —
//
//     #0 bz_string_release
//     #1 bz$exDisposeMsg
//     #2 bz$TsLib2867T1.MyEx2867$DisposeCore
//     ERROR: AddressSanitizer: SEGV on unknown address 0x2b   (rdi = 0x13)
//
// — because the two artifacts disagreed about the §31.4 header's offsets by 32
// bytes. So a single-module grid, however wide, could not have told anyone this
// fix was safe. Both halves of #2951 (its own release, and #2950's layout
// carriage) are exercised here together, which is the only configuration in
// which either is sound.
//
// THE TWO ARMS, and why both are needed. `emitDisposerCore` gates the release on
// the TRANSITIVE `hasBuiltinExceptionBase`, so:
//
//   * an IMPORTED subclass is released by the EXPORTER's compiled core, reached
//     through the imported vtable; and
//   * a LOCAL class over an IMPORTED exception base is released by its OWN core,
//     which is this module's code — it does not depend on whether
//     `emitBaseDisposeChain` reaches into the dependency's compiled core, and
//     that independence is the point of the transitive gate.
//
// Both objects are allocated by the IMPORTER in the same program, so a fix that
// covered one and not the other leaks visibly.
//
// MEASURED (real `bezelc -c lib.bzl` then `bezelc --sanitize=address --import
// lib.o app.bzl`, 20 iterations of each shape with a churn allocation):
//   without the release   `1492 byte(s) leaked in 84 allocation(s)`, exit 1
//   with it               t=695, LSan-clean, exit 0
//
// Every message and paramName is built at RUNTIME (`"m" + n.ToString()`) inside
// the EXPORTER's constructor: a literal is an immortal sentinel on which
// release is a no-op, which would make the whole probe vacuous. The printed
// total is checked exactly, so a "fix" that dropped or emptied a slot rather
// than releasing it fails here too — and it is the ASan/LSan verdict, not the
// total, that carries the lifetime claim (a leaked block still holds its bytes).

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
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2951 {
    std::string path;
    explicit TempFile2951(std::string p) : path(std::move(p)) {}
    ~TempFile2951() { std::remove(path.c_str()); }
};

void writeSource2951(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build/link or did not exit normally. Generated-code ASan is on for the
// importer WITH LeakSanitizer — the leak is this issue, and LSan reports it by
// forcing a non-zero exit, so `exit == 0` IS the under-release assertion. The
// dependency object is compiled without instrumentation (`compileObject` takes
// no sanitize option), which is why the exporter's own OOB write was invisible
// before #2950 and why the layout half had to land first.
int compileImportAndRunAsan2951(const std::string& libSrc,
                                const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2951_");
    TempFile2951 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2951 aObj((dir / (stem + "_a.o")).string());
    writeSource2951(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2951 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2951 bExe((dir / stem).string());
    writeSource2951(bSrc.path, appSrc);
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
// X1 — both arms plus the ParamName slot, in one importer-side program.
//
//   * `XEx2951` — an IMPORTED subclass, allocated and disposed by the importer,
//     released by the EXPORTER's compiled core.
//   * `LocalOver2951: XEx2951` — a LOCAL class over that imported exception
//     base, released by its OWN core under the transitive gate.
//   * `XArg2951: ArgumentException` — the two-argument overload, so the
//     ParamName slot at byte offset 32 is exercised across the boundary too;
//     that offset is 16 bytes past the end of the pre-#2950 importer block, so
//     this arm is also the one that could not even be allocated correctly
//     before.
//
// Pre-fix, MEASURED: exit 1 with `1492 byte(s) leaked in 84 allocation(s)` —
// the harness sees a non-zero exit, so this cell fails LOUDLY.
// ===========================================================================
BEZEL_TEST(xmod_exception_subclass_header_release_2951,
           imported_and_local_over_imported_release_their_31_4_header) {
    const char* lib =
        "module ExLib2951X; "
        "public class XEx2951: Exception "
        "  public V: int; "
        "  public constructor(n: int) base(\"m\" + n.ToString()); "
        "    V := n; "
        "  end constructor; "
        "end XEx2951; "
        "public class XArg2951: ArgumentException "
        "  public constructor(n: int) "
        "      base(\"a\" + n.ToString(), \"p\" + n.ToString()); "
        "  end constructor; "
        "end XArg2951;";
    int r = compileImportAndRunAsan2951(
        lib,
        "module ExApp2951X; import ExLib2951X; "
        "public class LocalOver2951: XEx2951 "
        "  public L: int; "
        "  public constructor(n: int) base(n); "
        "    L := n + 1; "
        "  end constructor; "
        "end LocalOver2951; "
        "public class C2951X "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      using var e: XEx2951 := new XEx2951(i); "
        "      t := t + e.V + e.Message.Length; "
        "      using var l: LocalOver2951 := new LocalOver2951(i); "
        "      t := t + l.L + l.Message.Length; "
        "      using var a: XArg2951 := new XArg2951(i); "
        "      var ab: ArgumentException := a; "
        "      var pn: string? := ab.ParamName; "
        // §7.4 narrowing (§31.4 declares ParamName as `string?`). A null here
        // would leave the total short, so the guard cannot silently skip it.
        "      if pn <> null then "
        "        t := t + pn.Length; "
        "      end if; "
        "      var churn: string := \"c\" + i.ToString(); "
        "      t := t + a.Message.Length + churn.Length; "
        "    end while; "
        // 210 (e.V) + 51 (e.Message) + 230 (l.L) + 51 (l.Message)
        // + 51 (ParamName) + 51 (a.Message) + 51 (churn) = 695.
        "    if t <> 695 then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end C2951X;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

}  // namespace bezel::module
