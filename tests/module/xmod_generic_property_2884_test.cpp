// fix #2884 (§14.1 "Generics" / §2.8.3 rule 4 / §2.8.2 clause 1, v0.240,
// decline-with-reasons): a property on an IMPORTED GENERIC class had no working
// access path in ANY spelling, and the four failure modes were all illegible.
//
// MEASURED ON THE PARENT COMMIT (2aed89a / 32bbd98), on real two-module builds
// (`bezelc -c lib.bzl` then `bezelc --import lib.o app.bzl`):
//
//   g.P            (computed property, read)   -> undefined reference to
//                                                 `bz$HLib.HG$P$propget`
//                                                 — a hard LINK error
//   g.P := 7;      (computed property, write)  -> undefined reference to
//                                                 `bz$HLib.HG$P$propset`
//   a.A / a.A := v (AUTO-property)              -> undefined reference to
//                                                 `bz$L5.GAuto$A$propget` /
//                                                 `$propset`
//   v.V / v.V := 7 (VIRTUAL property)           -> SIGSEGV at runtime
//   g.P += 1; g.P++;                            -> compiled CLEAN, write
//                                                  SILENTLY DROPPED
//
// (The issue reported the SETTER link error and INFERRED the getter's; the
// getter half is reproduced here — it is the same hard link error, and it is
// the one that aborts the build first. The AUTO and VIRTUAL shapes were found
// by this fix's own grid and are not in the issue text at all.)
//
// ROOT CAUSE: `emitExportedProperty{Getters,Setters}` define the accessor stub
// only for a NON-generic type — a generic type's accessor is per-instantiation
// (§2.8.3 rule 4: "a constructed generic type … is one type in the running
// process", and the exporter cannot name the importer's instantiations) —
// while `declareImportedProperty{Getters,Setters}` carry no generic gate and
// emit the external reference anyway. The two halves of the ABI disagree.
//
// THE FIX SHIPPED HERE IS A DECLINE, NOT THE FULL CAPABILITY, and it is scoped
// deliberately: rejecting the access at its source with a sourced diagnostic,
// mirroring the #783 gate for an imported generic class's METHOD ("not yet
// monomorphized (§14.1 / #783)") in both form and reasoning. It is a strict
// improvement on all four measured outcomes — a link error naming a mangled
// symbol, and a runtime crash, become a diagnostic at the access site; the
// silently dropped `+=`/`++` write becomes loud (CLAUDE.md: "a clean compile
// error beats a silent wrong value") — and it rejects NOTHING that works
// today: every shape above is broken NOW, and the working neighbours are
// outside the gate (T6/T7/T8 pin them).
//
// WHY NOT THE REAL FIX HERE: making the access WORK needs the accessor bodies
// carried in the artifact and re-instantiated importer-side, the way
// `methodBodyText`/`ctorBodyText`/`fieldInitText` already carry method,
// constructor and static-field-initializer source for generic classes
// (ModuleMetadata.hpp schema v5+, ModuleLoader's parseAndAttachMethodBody).
// The cheap-looking alternative — exporting one placeholder-layout stub for the
// open type, which is what the generic class's METHODS fall back to — was
// prototyped and MEASURED to be unsound: with `class Off<T> _t: T; _s: int;`
// (T first, so every later field moves when T is a reference type), the
// exported stub read and wrote `_s` at the placeholder offset, so `Off<string>`
// answered `str_p=100` / `str_p2=8` where 108 / 21 are correct — a SILENT wrong
// value, and worse than the link error it replaced. The same program's METHOD
// calls were correct (100/8), because those ARE reconstructed importer-side.
// That measurement is why this commit declines instead of exporting stubs.
//
// FOLLOW-UP (fix #2893, v0.240): the propertyBodyText carriage
// (ModuleMetadataBuilder's propertyBodyText span-extraction +
// ModuleLoader's parseAndAttachPropertyBody, the property/indexer twin of
// parseAndAttachMethodBody) + the narrowed #2884 decline (now firing
// only for VIRTUAL/OVERRIDE/ABSTRACT/STATIC shapes) makes every COMPUTED /
// BLOCK access on an imported generic class actually WORK. T1-T5 below are
// turned from decline assertions into value assertions: pre-#2893 they
// failed at LINK with `undefined reference to bz$...$propget` (T1/T2/T4)
// or compiled clean and crashed (T5) or silently dropped the write (T3);
// post-#2893 they build, link, run, and return the right values. The new
// capability's grid is in tests/module/xmod_property_monomorphization_
// 2893_test.cpp (T1-T5 there are the same shapes as T1-T5 here); the
// sibling test file is kept here so the historical record of the decline
// and its narrow carve-out is preserved.
//
// FAIL-WITHOUT/PASS-WITH: with src/ reverted to HEAD, T1-T5 fail (T1/T2/T4
// fail at LINK with no such diagnostic; T3 compiles clean, i.e. no diagnostic
// at all; T5 compiles clean and crashes) and T6-T8 pass; with the fix, all
// pass.

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

// Per-process unique working dir under the shared scratch base. The previous
// fs::temp_directory_path() (== /tmp tmpfs) was race-prone even with
// unique_stem: parallel shards racing on the SAME /tmp parent dir overwrote
// each other's _a.bzl / _a.o / _b.bzl mid-compileObject. The
// /var/tmp/bezel-scratch parent fixes that class of flake (#3002-class
// COVERAGE SHORT). popen() at L179 is kept — per-stem uniqueness already
// mitigates the file-overwrite race; full fork isolation is a separate concern.
const std::filesystem::path& wtDir2884() {
    return wtScratchDir("wt-2884");
}

struct TempFile2884 {
    std::string path;
    explicit TempFile2884(std::string p) : path(std::move(p)) {}
    ~TempFile2884() { std::remove(path.c_str()); }
};

void writeSource2884(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile `libSrc` to an object, then compile `appSrc` against it with the
// driver's stderr captured, so the test can assert on the diagnostic TEXT
// rather than on failure alone: pre-fix these programs ALSO fail to build (at
// LINK, with `undefined reference to bz$…$propget`), so "the compile failed" is
// not evidence of anything. Returns {compileSucceeded, stderr}; `ok` is set
// false with an empty string when the dependency itself could not be produced
// (toolchain unavailable -> SKIP).
struct BuildResult2884 { bool depOk; bool success; std::string err; };

BuildResult2884 compileImportCaptureErr2884(const std::string& libSrc,
                                            const std::string& appSrc,
                                            const std::string& tag) {
    auto dir = wtDir2884();
    std::string stem = "bz2884_" + tag + "_" + unique_stem("");
    TempFile2884 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2884 aObj((dir / (stem + "_a.o")).string());
    writeSource2884(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return {false, false, ""};
    }
    if (!std::ifstream(aObj.path).good()) return {false, false, ""};

    TempFile2884 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2884 bExe((dir / (stem + "_b")).string());
    writeSource2884(bSrc.path, appSrc);

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

// Compile lib + app, link and run, capturing stdout — for the ACCEPT controls.
std::pair<int, std::string> compileImportRun2884(const std::string& libSrc,
                                                 const std::string& appSrc,
                                                 const std::string& tag) {
    auto dir = wtDir2884();
    std::string stem = "bz2884r_" + tag + "_" + unique_stem("");
    TempFile2884 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2884 aObj((dir / (stem + "_a.o")).string());
    writeSource2884(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return {-2, ""};
    }
    if (!std::ifstream(aObj.path).good()) return {-2, ""};
    TempFile2884 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2884 bExe((dir / (stem + "_b")).string());
    writeSource2884(bSrc.path, appSrc);
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

bool has2884(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// The diagnostic's load-bearing fragments (asserted rather than the whole
// sentence, so re-wording the advice does not break the pin).
void expectDecline2884(const std::string& err, const std::string& prop) {
    BEZEL_EXPECT_TRUE(has2884(err, "property `" + prop + "`"));
    BEZEL_EXPECT_TRUE(has2884(err, "imported generic type"));
    BEZEL_EXPECT_TRUE(has2884(err, "not yet monomorphized"));
    BEZEL_EXPECT_TRUE(has2884(err, "#2884"));
    // NOT the linker's mangled-symbol failure — that is the pre-fix outcome.
    BEZEL_EXPECT_FALSE(has2884(err, "undefined reference to `bz$"));
}

const char* kGenLib2884 =
    "module GenLib2884; "
    "public class GBox2884<T> "
    "  public _s: int := 0; "
    "  public _t: T; "
    "  public property P: int "
    "    get => _s + 100; "
    "    set => do _s := value + 1; end; "
    "  end; "
    "  public method GetS(): int "
    "    return _s + 100; "
    "  end GetS; "
    "  public method SetS(v: int) "
    "    _s := v + 1; "
    "  end SetS; "
    "end GBox2884;";

}  // namespace

// ===========================================================================
// T1 — the READ half, which the issue could only INFER (the setter's link
// error aborts the build first). Reproduced: it is the same hard link error.
// Post-#2893 the read lowers through the re-parsed PropertyDecl with
// `genericSubst_` active, evaluates `_s + 100` at the imported
// instantiation's layout, and returns 100.
// ===========================================================================
BEZEL_TEST(xmod_generic_property_2884, computed_property_read_works) {
    auto [code, out] = compileImportRun2884(
        kGenLib2884,
        "module GenApp2884T1; import GenLib2884; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GBox2884<int>(); "
        "    Console.WriteLine(\"p=\" + g.P.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t1");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2884(out, "p=100"));
}

// ===========================================================================
// T2 — the issue's own repro: the plain `:=` write. Post-#2893 the setter
// lowers through the re-parsed BLOCK accessor, executes `_s := value + 1`,
// and the read returns 8.
// ===========================================================================
BEZEL_TEST(xmod_generic_property_2884, computed_property_assign_works) {
    auto [code, out] = compileImportRun2884(
        kGenLib2884,
        "module GenApp2884T2; import GenLib2884; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GBox2884<int>(); "
        "    g.P := 7; "
        "    Console.WriteLine(\"p=\" + g.P.ToString()); "
        "    Console.WriteLine(\"s=\" + g._s.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t2");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2884(out, "p=108"));
    BEZEL_EXPECT_TRUE(has2884(out, "s=8"));
}

// ===========================================================================
// T3 — the loudest improvement: `+=` and `++` (§6.6 / §17.6). Pre-fix they
// COMPILED CLEAN and silently discarded the write (#2862's lowering
// declined for a generic parent precisely to avoid the T1/T2 link error).
// Post-#2893 the lowering type-checks against the re-parsed PropertyDecl,
// and the compound write / increment correctly emits and applies the
// inline setter body.
// ===========================================================================
BEZEL_TEST(xmod_generic_property_2884, compound_and_increment_works) {
    auto [code, out] = compileImportRun2884(
        kGenLib2884,
        "module GenApp2884T3; import GenLib2884; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GBox2884<int>(); "
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
    BEZEL_EXPECT_TRUE(has2884(out, "add=102"));
    BEZEL_EXPECT_TRUE(has2884(out, "inc=204"));
}

// ===========================================================================
// T4 — an AUTO-property on an imported generic class. Pre-#2893 the
// #2478 instance-storage path does not engage for a generic receiver (the
// exporter exports `isAutoProperty=true` but the accessor stub is skipped),
// so the access routed to the never-defined accessor stub at LINK. Post-
// #2893 the #2478 instance-storage path engages correctly: `A` reads the
// backing field, returning the initializer value.
// ===========================================================================
BEZEL_TEST(xmod_generic_property_2884, auto_property_works) {
    auto [code, out] = compileImportRun2884(
        "module GenLib2884T4; "
        "public class GAuto2884<T> "
        "  public _t: T; "
        "  public property A: int := 3; "
        "end GAuto2884;",
        "module GenApp2884T4; import GenLib2884T4; "
        "public class C "
        "  public static method Main(): int "
        "    using var a := new GAuto2884<int>(); "
        "    Console.WriteLine(\"a=\" + a.A.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t4");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2884(out, "a=3"));
}

// ===========================================================================
// T5 — a VIRTUAL property on an imported generic class, accessed through the
// CLASS type. Pre-#2893 this COMPILED CLEAN and then SIGSEGV'd (the
// instantiation's vtable slot was not populated). Post-#2893 the narrow
// decline still fires for the VIRTUAL arm (the exporter skips the virtual
// accessor stub, so the symbol genuinely does not exist); the access is
// rejected with the narrowed #2884 diagnostic.
// ===========================================================================
BEZEL_TEST(xmod_generic_property_2884, virtual_property_on_generic_declined) {
    auto r = compileImportCaptureErr2884(
        "module GenLib2884T5; "
        "public class GVirt2884<T> "
        "  public _t: T; "
        "  public _v: int := 0; "
        "  public virtual property V: int "
        "    get => _v + 100; "
        "    set => do _v := value + 1; end; "
        "  end; "
        "end GVirt2884;",
        "module GenApp2884T5; import GenLib2884T5; "
        "public class C "
        "  public static method Main(): int "
        "    using var v := new GVirt2884<int>(); "
        "    return v.V; "
        "  end Main; "
        "end C;",
        "t5");
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    expectDecline2884(r.err, "V");
}

// ===========================================================================
// T6 — ACCEPT CONTROL (passes before and after): FIELDS and METHODS on the very
// same imported generic class keep working, at BOTH a value-type and a
// reference-type instantiation. This is the line the decline must not cross —
// a method body IS reconstructed importer-side (#783 methodBodyText), which is
// exactly why properties, which are not, had to be declined instead.
// ===========================================================================
BEZEL_TEST(xmod_generic_property_2884, fields_and_methods_still_work) {
    auto [code, out] = compileImportRun2884(
        kGenLib2884,
        "module GenApp2884T6; import GenLib2884; "
        "public class C "
        "  public static method Main(): int "
        "    using var gi := new GBox2884<int>(); "
        "    Console.WriteLine(\"m=\" + gi.GetS().ToString()); "
        "    gi.SetS(7); "
        "    Console.WriteLine(\"f=\" + gi._s.ToString()); "
        "    gi._s := 20; "
        "    gi._s += 1; "
        "    Console.WriteLine(\"f2=\" + gi._s.ToString()); "
        "    using var gs := new GBox2884<string>(); "
        "    Console.WriteLine(\"sm=\" + gs.GetS().ToString()); "
        "    gs.SetS(7); "
        "    Console.WriteLine(\"sf=\" + gs._s.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t6");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2884(out, "m=100"));
    BEZEL_EXPECT_TRUE(has2884(out, "f=8"));
    BEZEL_EXPECT_TRUE(has2884(out, "f2=21"));
    BEZEL_EXPECT_TRUE(has2884(out, "sm=100"));
    BEZEL_EXPECT_TRUE(has2884(out, "sf=8"));
}

// ===========================================================================
// T7 — ACCEPT CONTROL: a NON-generic imported class's computed property (the
// shape #2400/#2862 fixed) is untouched — read, `:=`, `+=` and `++` all still
// go through the exported instance accessor stubs and answer correctly.
// ===========================================================================
BEZEL_TEST(xmod_generic_property_2884, non_generic_imported_property_unchanged) {
    auto [code, out] = compileImportRun2884(
        "module GenLib2884T7; "
        "public class HPlain2884 "
        "  public _n: int := 1; "
        "  public property Name: int "
        "    get => _n + 100; "
        "    set => do _n := value + 1; end; "
        "  end; "
        "end HPlain2884;",
        "module GenApp2884T7; import GenLib2884T7; "
        "public class C "
        "  public static method Main(): int "
        "    using var h := new HPlain2884(); "
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
        "t7");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2884(out, "get=101"));
    BEZEL_EXPECT_TRUE(has2884(out, "set=8"));
    BEZEL_EXPECT_TRUE(has2884(out, "cmp=110"));
    BEZEL_EXPECT_TRUE(has2884(out, "inc=212"));
}

// ===========================================================================
// T8 — ACCEPT CONTROL: the SAME generic class's property, used inside its OWN
// module, is fully correct in every spelling. This is what §2.8.2 clause 2
// ("an external instantiation behaves identically to a same-module
// instantiation") demands of the eventual real fix, and it is the pin that the
// decline is scoped to the IMPORTED case only.
// ===========================================================================
BEZEL_TEST(xmod_generic_property_2884, same_module_generic_property_correct) {
    auto [code, out] = compileImportRun2884(
        "module GenLib2884T8; "
        "public class Unused2884 "
        "  public n: int := 0; "
        "end Unused2884;",
        "module GenApp2884T8; import GenLib2884T8; "
        "public class GLocal2884<T> "
        "  public _s: int := 0; "
        "  public _t: T; "
        "  public property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end GLocal2884; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GLocal2884<string>(); "
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
        "t8");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has2884(out, "get=100"));
    BEZEL_EXPECT_TRUE(has2884(out, "set=8"));
    BEZEL_EXPECT_TRUE(has2884(out, "cmp=110"));
    BEZEL_EXPECT_TRUE(has2884(out, "inc=212"));
}

}  // namespace bezel::module
