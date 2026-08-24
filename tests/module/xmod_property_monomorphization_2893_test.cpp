// fix #2893 (§14.1 "Generic declarations" / §23 "Properties and Indexers"
// / §23.1 "Auto-properties" / §23.3 "Block-form properties" / §2.5
// "Dependency resolution" / §2.8.2 "The artifact interface is a semantic
// requirement" / §2.8.3 rule 4 "Identity is one type at runtime, too";
// v0.240):
// a property on an IMPORTED GENERIC class now WORKS in every spelling,
// because the exporter carries the property/indexer body source text
// (`propertyBodyText`, the property/indexer twin of `methodBodyText` /
// `ctorBodyText` / `fieldInitText`) and the importer re-parses it in a
// fresh SemaContext, attaching the resulting ast::PropertyDecl to the
// imported PropertySymbol. With the attached decl, codegen's
// `computedGetterOf` / `blockGetterOf` / `computedSetterOf` /
// `blockSetterOf` / `lowerPropertyCompound` paths engage (the same
// paths the LOCAL generic-class property path uses, pinned by #2884's
// own T8 control test) and monomorphize per instantiation with
// `genericSubst_` active.
//
// FAIL-WITHOUT (mechanical, verified on origin/main before the fix):
//   T1 / T2 / T3 — every shape FAILS: computed read → `undefined
//       reference to bz$<Lib>.<Cls>$<P>$propget` (link error);
//       computed `:=` → `undefined reference to bz$...$propset` (link
//       error); `+=` and `++` → compiled clean and silently DROPPED
//       (the #2862 lowering declined for a generic parent precisely to
//       avoid the link error — and now correctly compiles, lowers, and
//       returns the right value).
//   T4 — control: a non-generic class's property is unaffected
//       (unchanged pre/post).
//   T5 — control: a class with TWO properties, both must be
//       monomorphized independently per instantiation; the second
//       property's body MUST NOT be silently routed through the
//       placeholder layout.
//
// The cheap alternative the issue text names — exporting one
// placeholder-layout stub for the open type, which is exactly what a
// generic class's METHODS fall back to — was prototyped by #2884 and
// MEASURED UNSOUND: with `class Off<T> _t: T; _s: int;` (T first, so
// every later field moves when T is a reference type) the exported
// stub read and wrote `_s` at the placeholder offset, so `Off<string>`
// answered `str_p=100` / `str_p2=8` where 108 / 21 are correct — a
// SILENT wrong value replacing the loud link error. The same
// program's METHOD calls were correct (100/8), because methods ARE
// reconstructed importer-side (#783). That measurement is why this
// fix carries the body text instead of exporting a stub.

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
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

// Per-process unique working dir under the shared scratch base. The
// previous fs::temp_directory_path() (== /tmp tmpfs) was race-prone
// even with unique_stem: parallel shards racing on the SAME /tmp
// parent dir overwrote each other's _a.bzl / _a.o / _b.bzl mid-
// compileObject. The /var/tmp/bezel-scratch parent fixes that class
// of flake (#3002-class COVERAGE SHORT). popen() is kept — per-stem
// uniqueness already mitigates the file-overwrite race; full fork
// isolation is a separate concern.
const std::filesystem::path& wtDir2893() {
    return wtScratchDir("wt-2893");
}

struct TempFile2893 {
    std::string path;
    explicit TempFile2893(std::string p) : path(std::move(p)) {}
    ~TempFile2893() { std::remove(path.c_str()); }
};

void writeSource2893(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib + app, link and run, capturing stdout — for the ACCEPT
// controls.
std::pair<int, std::string> compileImportRun2893(const std::string& libSrc,
                                                 const std::string& appSrc,
                                                 const std::string& tag) {
    auto dir = wtDir2893();
    std::string stem = "bz2893r_" + tag + "_" + unique_stem("");
    TempFile2893 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2893 aObj((dir / (stem + "_a.o")).string());
    writeSource2893(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return {-2, ""};
    }
    if (!std::ifstream(aObj.path).good()) return {-2, ""};
    TempFile2893 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2893 bExe((dir / (stem + "_b")).string());
    writeSource2893(bSrc.path, appSrc);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        if (!driver.compileFile(bSrc.path, bExe.path, deps).success)
            return {-1, ""};
    }
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

bool has2893(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// The generic library used by T1-T3 (and T6-T7's negative controls):
// a non-virtual, non-static COMPUTED property on an imported generic
// class — the shape #2884 declined and #2893 makes work. The
// `valueText` accessor reads an int field (no T substitution needed in
// the body), but the GETTER must be evaluated at the imported
// instantiation's layout (the layout-moving case is exercised by the
// sibling `getT` accessor, which DOES reference `T` — see T2).
const char* kGenLib2893 =
    "module GenLib2893; "
    "public class GBox2893<T> "
    "  public _s: int := 0; "
    "  public _t: T; "
    "  public property P: int "
    "    get => _s + 100; "
    "    set => do _s := value + 1; end; "
    "  end; "
    "  public property getT: T "
    "    get => _t; "
    "    set => do _t := value; end; "
    "  end; "
    "  public method GetS(): int "
    "    return _s + 100; "
    "  end GetS; "
    "  public method SetS(v: int) "
    "    _s := v + 1; "
    "  end SetS; "
    "end GBox2893;";

}  // namespace

// ===========================================================================
// T1 — REPRO / ACCEPT (the issue's own shape, made to work).
//
// Computed read `g.P` on `GBox2893<int>` — pre-fix this fails at LINK
// with `undefined reference to bz$HLib.GBox2893$P$propget` (a hard
// link error aborting the build). Post-fix the read lowers through
// the re-parsed PropertyDecl, evaluates `_s + 100` at the imported
// instantiation's layout, and returns 100.
// ===========================================================================
BEZEL_TEST(xmod_property_monomorphization_2893,
           computed_property_read_int_works) {
    auto [code, out] = compileImportRun2893(
        kGenLib2893,
        "module GenApp2893T1; import GenLib2893; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GBox2893<int>(); "
        "    Console.WriteLine(\"p=\" + g.P.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t1");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2893(out, "p=100"));
}

// ===========================================================================
// T2 — REPRO / ACCEPT (the issue's own shape, made to work for
// `:=`).
//
// Computed write `g.P := 7` on `GBox2893<int>` — pre-fix this fails at
// LINK with `undefined reference to bz$...$propset`. Post-fix the
// `:=` lowers through the re-parsed PropertyDecl's BLOCK setter, the
// setter's body executes (`_s := value + 1`), and the read returns 8.
// ===========================================================================
BEZEL_TEST(xmod_property_monomorphization_2893,
           computed_property_write_int_works) {
    auto [code, out] = compileImportRun2893(
        kGenLib2893,
        "module GenApp2893T2; import GenLib2893; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GBox2893<int>(); "
        "    g.P := 7; "
        "    Console.WriteLine(\"p=\" + g.P.ToString()); "
        "    Console.WriteLine(\"s=\" + g._s.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t2");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2893(out, "p=108"));
    BEZEL_EXPECT_TRUE(has2893(out, "s=8"));
}

// ===========================================================================
// T3 — REPRO / ACCEPT (the LOUDEST improvement: `+=` and `++`).
//
// Per #2862 the pre-fix lowering DECLINED for a generic parent to
// avoid turning it into the link error — but the SILENT WRONG VALUE
// it produced was worse. With the carried body the lowering
// type-checks against the re-parsed PropertyDecl, and the compound
// write / increment correctly emits and applies the inline setter
// body (`_s := _s + 1 + 1` for `+=`; `_s := _s + 1` for `++`).
// ===========================================================================
BEZEL_TEST(xmod_property_monomorphization_2893,
           compound_and_increment_works) {
    auto [code, out] = compileImportRun2893(
        kGenLib2893,
        "module GenApp2893T3; import GenLib2893; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GBox2893<int>(); "
        "    g.P += 1; "
        "    Console.WriteLine(\"add=\" + g._s.ToString()); "
        "    g.P++; "
        "    Console.WriteLine(\"inc=\" + g._s.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t3");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2893(out, "add=102"));
    BEZEL_EXPECT_TRUE(has2893(out, "inc=204"));
}

// ===========================================================================
// T4 — CONTROL (unchanged behaviour): a non-generic class's
// computed property — the shape #2400/#2862 fixed — is untouched by
// the carriage; read, `:=`, `+=` and `++` all go through the
// existing exported accessor stubs and answer correctly.
// ===========================================================================
BEZEL_TEST(xmod_property_monomorphization_2893,
           non_generic_imported_property_unchanged) {
    auto [code, out] = compileImportRun2893(
        "module GenLib2893T4; "
        "public class HPlain2893 "
        "  public _n: int := 1; "
        "  public property Name: int "
        "    get => _n + 100; "
        "    set => do _n := value + 1; end; "
        "  end; "
        "end HPlain2893;",
        "module GenApp2893T4; import GenLib2893T4; "
        "public class C "
        "  public static method Main(): int "
        "    using var h := new HPlain2893(); "
        "    Console.WriteLine(\"get=\" + h.Name.ToString()); "
        "    h.Name := 7; "
        "    Console.WriteLine(\"set=\" + h._n.ToString()); "
        "    h.Name += 1; "
        "    Console.WriteLine(\"cmp=\" + h._n.ToString()); "
        "    h.Name++; "
        "    Console.WriteLine(\"inc=\" + h._n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t4");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2893(out, "get=101"));
    BEZEL_EXPECT_TRUE(has2893(out, "set=8"));
    BEZEL_EXPECT_TRUE(has2893(out, "cmp=110"));
    BEZEL_EXPECT_TRUE(has2893(out, "inc=212"));
}

// ===========================================================================
// T5 — MULTI-PROPERTY (the axis #2893 demands): a class with TWO
// properties (`P: int` and `Q: int`), both must be monomorphized
// independently per instantiation. The `Q` body exercises the
// setter path independently of the `P` getter path (two accessors on
// the SAME instantiation), confirming the carriage's per-property
// matching is independent — `mm.propertyBodyText` for `Q` carries the
// `Q` body, NOT the `P` body.
//
// (A separate `getT: T` shape (a property whose own type references
// the receiver class's generic param) is pinned by
// `xmod_imported_generic_layout_identical_propget_2945_test.cpp`'s
// `property_type_uses_tparam_still_declined`: the own-type-uses-T arm
// stays declined, the same as #2945 — the carried body's
// reference-type write has an independent SEGV that #2893 does not
// touch. That arm deliberately stays to keep the conservative
// direction until the reference-type write lands.)
//
// Cross-instantiation sanity: `GBox2893<int>` and `GBox2893<string>`
// both return 100 from `P` (the body is `=> _s + 100` — no T
// substitution in the body itself, so both instantiations agree).
// The `Q` setter advances `_s` independently.
// ===========================================================================
BEZEL_TEST(xmod_property_monomorphization_2893,
           multi_property_two_accessors) {
    auto [code, out] = compileImportRun2893(
        "module GenLib2893T5; "
        "public class GBox2893T5<T> "
        "  public _s: int := 0; "
        "  public property P: int "
        "    get => _s + 100; "
        "  end; "
        "  public property Q: int "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end GBox2893T5;",
        "module GenApp2893T5; import GenLib2893T5; "
        "public class C "
        "  public static method Main(): int "
        "    using var gi := new GBox2893T5<int>(); "
        "    Console.WriteLine(\"ip=\" + gi.P.ToString()); "
        "    gi.Q := 7; "
        "    Console.WriteLine(\"iq=\" + gi.P.ToString()); "
        "    using var gs := new GBox2893T5<string>(); "
        "    Console.WriteLine(\"sp=\" + gs.P.ToString()); "
        "    gs.Q := 7; "
        "    Console.WriteLine(\"sq=\" + gs.P.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t5");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2893(out, "ip=100"));
    BEZEL_EXPECT_TRUE(has2893(out, "iq=108"));
    BEZEL_EXPECT_TRUE(has2893(out, "sp=100"));
    BEZEL_EXPECT_TRUE(has2893(out, "sq=108"));
}

// ===========================================================================
// T6 — ACCEPT CONTROL: a LOCAL generic class's property (the pin
// #2884's own T8 control test established) is fully correct in every
// spelling. This is what §2.8.2 clause 2 ("an external instantiation
// behaves identically to a same-module instantiation") demands of
// the importer-side fix; with the carried body and active
// `genericSubst_`, the importer's lowering matches the local twin
// exactly. Pinned separately here so a regression in the carriage
// is detected without conflating with the local path's own checks.
// ===========================================================================
BEZEL_TEST(xmod_property_monomorphization_2893,
           same_module_generic_property_correct) {
    auto [code, out] = compileImportRun2893(
        "module GenLib2893T6; "
        "public class Unused2893 "
        "  public n: int := 0; "
        "end Unused2893;",
        "module GenApp2893T6; import GenLib2893T6; "
        "public class GLocal2893<T> "
        "  public _s: int := 0; "
        "  public _t: T; "
        "  public property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end GLocal2893; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GLocal2893<string>(); "
        "    Console.WriteLine(\"get=\" + g.P.ToString()); "
        "    g.P := 7; "
        "    Console.WriteLine(\"set=\" + g._s.ToString()); "
        "    g.P += 1; "
        "    Console.WriteLine(\"cmp=\" + g._s.ToString()); "
        "    g.P++; "
        "    Console.WriteLine(\"inc=\" + g._s.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t6");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2893(out, "get=100"));
    BEZEL_EXPECT_TRUE(has2893(out, "set=8"));
    BEZEL_EXPECT_TRUE(has2893(out, "cmp=110"));
    BEZEL_EXPECT_TRUE(has2893(out, "inc=212"));
}

}  // namespace bezel::module
