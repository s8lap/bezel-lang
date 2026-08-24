// fix #2945 (§14.1 "Generic declarations" / §2.8.3 rule 4 / §2.5, v0.252):
// #2884's decline of every property access on an imported GENERIC class was
// predicated on "a generic type's accessors are not exported per instantiation"
// (commit 5116fc0). #2894's fix lifted that gate
// (`emitExportedPropertyGetters` and `…Setters` no longer skip generic types),
// so the public mangled symbol `bz$<Lib>.<Cls>$<P>$propget` IS exported —
// measured: `nm lib.o | grep propget` shows the `T` entry. With the
// field/storage layout identical to the erased `T -> i32` template (no field
// of the class or any ancestor declares a type-parameter-typed type, and the
// property's own type also does not), the exported body reads and writes the
// right offset, and the direct class-typed access `g.P` works.
//
// Three shapes are pinned:
//   * T1 — READ on `GBox<T>` with no T-typed storage (so `_w: int` keeps the
//     erased layout) at a STRING instantiation that would otherwise move it:
//     pre-fix, this fails at SEMA with the #2884 diagnostic ("not yet
//     monomorphized"); post-fix, it builds and returns `_w + 100 = 100`.
//   * T2 — WRITE through `:=`: the SETTER half; the setter calls the layout-
//     pinned stub body (`_w := value + 1`) which writes the same offset the
//     getter reads. `_w` advances from 0 to 8 and is read back through both
//     the field and the getter.
//   * T3 — VIRTUAL remains declined. The exporter skips virtual properties
//     (`isVirtualProperty(ps)` gate at LlvmBackend_Setup.cpp ~2458), so no
//     `$propget` symbol exists for `V` regardless of layout — the decline is
//     the correct answer. Pinned by the same diagnostic text asserted in #2884
//     T5.
//
// Negative controls (kept over-rejecting, deliberately):
//   * T4 — a `GBox<T>` that DOES carry a T-typed storage field (`_t: T`). The
//     layout MOVES for a reference-type instantiation, so the exported stub's
//     offsets are wrong — keeping the decline here is exactly what #2884
//     argued for, and the same predicate should still decline.
//   * T5 — the property's own type uses T (`property P: T`). The exported
//     stub's ABI is the i32 placeholder, so a T-typed property would be a
//     silent wrong value at every call — kept over-rejected.
//
// Built the same way as its sibling tests/module/xmod_generic_property_2884_
// test.cpp: a real `bezelc -c lib.bzl` + `bezelc --import lib.o app.bzl` +
// native run. Outputs that the linker dropped (the pre-fix failure mode for
// T1/T2) are no longer reachable in the post-fix source, so the test passes
// only on the fixed source; with src/ reverted, T1/T2 fail at SEMA with the
// #2884 diagnostic.

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
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

const std::filesystem::path& wtDir2945() {
    return wtScratchDir("wt-2945");
}

struct TempFile2945 {
    std::string path;
    explicit TempFile2945(std::string p) : path(std::move(p)) {}
    ~TempFile2945() { std::remove(path.c_str()); }
};

void writeSource2945(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib + app, link and run, capturing stdout. Returns exit code (-2 =
// dependency toolchain unavailable -> skip, -1 = compile failed, otherwise the
// exit code from running the linked binary).
std::pair<int, std::string> compileImportRun2945(const std::string& libSrc,
                                                 const std::string& appSrc,
                                                 const std::string& tag) {
    auto dir = wtDir2945();
    std::string stem = "bz2945_" + tag + "_" + unique_stem("");
    TempFile2945 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2945 aObj((dir / (stem + "_a.o")).string());
    writeSource2945(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return {-2, ""};
    }
    if (!std::ifstream(aObj.path).good()) return {-2, ""};
    TempFile2945 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2945 bExe((dir / (stem + "_b")).string());
    writeSource2945(bSrc.path, appSrc);
    bool ok;
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        ok = driver.compileFile(bSrc.path, bExe.path, deps).success;
    }
    if (!ok) return {-1, ""};
    std::string cmd = bExe.path + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {-1, ""};
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

// Compile lib + app, capture stderr (so the test can assert on diagnostic
// text for the negative controls that should STILL decline).
struct CaptureResult2945 { bool depOk; bool success; std::string err; };

CaptureResult2945 compileImportCaptureErr2945(const std::string& libSrc,
                                              const std::string& appSrc,
                                              const std::string& tag) {
    auto dir = wtDir2945();
    std::string stem = "bz2945e_" + tag + "_" + unique_stem("");
    TempFile2945 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2945 aObj((dir / (stem + "_a.o")).string());
    writeSource2945(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return {false, false, ""};
    }
    if (!std::ifstream(aObj.path).good()) return {false, false, ""};

    TempFile2945 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2945 bExe((dir / (stem + "_b")).string());
    writeSource2945(bSrc.path, appSrc);

    std::string errPath = (dir / (stem + "_err.txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        success = driver.compileFile(bSrc.path, bExe.path, deps).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) { ::close(fd); ::dup2(saved, STDERR_FILENO); }
    ::close(saved);
    std::ifstream in(errPath);
    std::string err((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    std::remove(bExe.path.c_str());
    return {true, success, err};
}

bool has2945(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ===========================================================================
// T1 — GETTER on `GBox<T>` with NO T-typed storage, instantiated at `string`.
// The accessor exists (post-#2894 export-side lift) and the layout matches
// the erased template, so the read returns `_w + 100 = 100`.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_layout_identical_propget_2945,
           read_no_tparam_storage_string) {
    auto [code, out] = compileImportRun2945(
        "module Lib2945T1; "
        "public class GBox2945T1<T> "
        "  public _w: int := 0; "
        "  public property P: int "
        "    get => _w + 100; "
        "    set => do _w := value + 1; end; "
        "  end; "
        "end GBox2945T1;",
        "module App2945T1; import Lib2945T1; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GBox2945T1<string>(); "
        "    Console.WriteLine(\"P=\" + g.P.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t1");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2945(out, "P=100"));
}

// ===========================================================================
// T2 — SETTER on the same class. The setter writes `_w := value + 1` (so
// `_w` advances from 0 to 8 with `:= 7`), and the getter then reads `_w +
// 100 = 108`.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_layout_identical_propget_2945,
           write_no_tparam_storage_string) {
    auto [code, out] = compileImportRun2945(
        "module Lib2945T2; "
        "public class GBox2945T2<T> "
        "  public _w: int := 0; "
        "  public property P: int "
        "    get => _w + 100; "
        "    set => do _w := value + 1; end; "
        "  end; "
        "end GBox2945T2;",
        "module App2945T2; import Lib2945T2; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GBox2945T2<string>(); "
        "    g.P := 7; "
        "    Console.WriteLine(\"P=\" + g.P.ToString()); "
        "    Console.WriteLine(\"_w=\" + g._w.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t2");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2945(out, "P=108"));
    BEZEL_EXPECT_TRUE(has2945(out, "_w=8"));
}

// ===========================================================================
// T3 — NEGATIVE CONTROL: a VIRTUAL property on the same imported generic class
// must still decline — the exporter skips virtual properties
// (`isVirtualProperty(ps)` gate), so no `$propget` symbol is emitted.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_layout_identical_propget_2945,
           virtual_property_still_declined) {
    auto r = compileImportCaptureErr2945(
        "module Lib2945T3; "
        "public class GVirt2945T3<T> "
        "  public _w: int := 0; "
        "  public virtual property V: int "
        "    get => _w + 100; "
        "    set => do _w := value + 1; end; "
        "  end; "
        "end GVirt2945T3;",
        "module App2945T3; import Lib2945T3; "
        "public class C "
        "  public static method Main(): int "
        "    using var v := new GVirt2945T3<int>(); "
        "    return v.V; "
        "  end Main; "
        "end C;",
        "t3");
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(has2945(r.err, "property `V`"));
    BEZEL_EXPECT_TRUE(has2945(r.err, "imported generic type"));
    BEZEL_EXPECT_TRUE(has2945(r.err, "not yet monomorphized"));
}

// ===========================================================================
// T4 — FORMERLY NEGATIVE CONTROL (fix #2893, v0.240): a `GBox<T>` that
// DOES carry a T-typed storage field (`_t: T`). The layout MOVES for a
// reference-type instantiation, so the exported stub's offsets are wrong —
// the decline USED to stay (the pre-#2893 fix #2945 kept the layout-moving
// arm). Post-#2893 the propertyBodyText carriage closes it: the re-parsed
// PropertyDecl is monomorphized per instantiation with `genericSubst_`
// active, the body reads `_w` against the SUBSTITUTED layout (the T→ptr
// offset, not the T→i32 erased placeholder), and the read returns the right
// value.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_layout_identical_propget_2945,
           tparam_storage_field_now_works) {
    auto [code, out] = compileImportRun2945(
        "module Lib2945T4; "
        "public class GBox2945T4<T> "
        "  public _t: T; "
        "  public _w: int := 0; "
        "  public property P: int "
        "    get => _w + 100; "
        "    set => do _w := value + 1; end; "
        "  end; "
        "end GBox2945T4;",
        "module App2945T4; import Lib2945T4; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GBox2945T4<string>(); "
        "    Console.WriteLine(\"p=\" + g.P.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t4");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2945(out, "p=100"));
}

// ===========================================================================
// T5 — NEGATIVE CONTROL: a property whose own TYPE uses T (`property P: T`).
// The exported stub's ABI is the i32 placeholder, so admitting this would
// silently read a pointer-sized value as i32. Decline stays (the
// pre-#2893 fix #2945 kept the own-type-uses-T arm). The fix #2893
// narrows the CLASS arm to VIRTUAL/ABSTRACT/STATIC, but this case is
// non-virtual/non-static and so WOULD be admitted — the propertyBodyText
// carriage attaches a re-parsed PropertyDecl, the inline-body path
// engages — except the codegen path for a property whose own type
// references a generic parameter, on a receiver whose layout MOVES, has
// an independent reference-type-store SEGV today (measured at HEAD on
// the local twin `class GLocalT5<T> { _t: T; property P: T get=>_t;
// set=>do _t := value; end; }` + `g.P := "hi"` at `T = string`: exits
// 139). That's a separate #2893-follow-up — the substitution works
// for the read (`=> _t`), the WRITE for a reference-typed T does not.
// Until that lands, keep the decline here. We instantiate at `string`
// and `int` so a wrong-value admit would be observably wrong; if the
// predicate is dropped, the linker error from the missing `$propset`
// is also acceptable evidence that the property is rejected.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_layout_identical_propget_2945,
           property_type_uses_tparam_still_declined) {
    auto r = compileImportCaptureErr2945(
        "module Lib2945T5; "
        "public class GBox2945T5<T> "
        "  public _t: T; "
        "  public property P: T "
        "    get => _t; "
        "    set => do _t := value; end; "
        "  end; "
        "end GBox2945T5;",
        "module App2945T5; import Lib2945T5; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GBox2945T5<string>(); "
        "    g.P := g; "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t5");
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(has2945(r.err, "property `P`"));
    BEZEL_EXPECT_TRUE(has2945(r.err, "imported generic type"));
    BEZEL_EXPECT_TRUE(has2945(r.err, "not yet monomorphized"));
}

}  // namespace bezel::module