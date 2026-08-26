// fix #3365 (§14.1 "Generic declarations" / §23.3 "Block-form properties" /
// §23.2 "Expression-bodied read-only properties" / §2.5 "Dependency
// resolution" / §2.8.2 "The artifact interface is a semantic requirement" /
// §2.8.3 rule 4 "Identity is one type at runtime, too", v0.282):
// a READ-ONLY property on an IMPORTED GENERIC class whose own declared type
// references the receiver class's generic param is now monomorphized per
// instantiation, mirroring the LOCAL generic-class property path (#2884's own
// T8 control test) and the imported-generic class property path #2893 closed
// for the int-typed case (existing
// `tests/module/xmod_property_monomorphization_2893_test.cpp`).
//
// ── THE DEFECT (this is what this test pins) ──────────────────────────────
//
// The pre-#3365 ExprTypecheck_Calls.cpp `declineUnsupportedImportedPropertyAccess`
// function declined every imported-generic class property access whose own
// declared type referenced a generic param (the "arm 3" gate, around line
// 13386 of the pre-fix source). The decl was rejected with
// `property `P` on the imported generic type `…` is not yet monomorphized
// (§14.1 / §2.8.3 rule 4 / #2884)`.
//
// That gate was right when the importer had no `ast::PropertyDecl` to drive
// the codegen — the only way to access the property was through the exported
// `bz$<Class>$<P>$propget` ABI stub, which carries the T→i32 erased
// placeholder layout, so a `T = string` (8-byte pointer) substitution would
// silently read the i32 stub's 4-byte return at the wrong offset.
//
// After #2893 (and the propertyBodyText carriage in ModuleMetadataBuilder.cpp +
// `parseAndAttachPropertyBody` in ModuleLoader.cpp), the importer ATTACHES a
// re-parsed `ast::PropertyDecl` to the imported `PropertySymbol` for any
// generic-class property with a non-trivial body. With the attached decl,
// codegen's `computedGetterOf` / `blockGetterOf` paths engage (the same
// paths the LOCAL generic-class property twin uses, pinned by #2884's own
// T8 control test) and monomorphize per instantiation with `genericSubst_`
// active — `T → Pt` in the body, the field read is correctly 8 bytes, the
// property's return type is the substituted concrete type.
//
// The pre-#3365 arm 3 gate over-rejected by NOT conditioning on whether the
// re-parsed body is attached. The post-#3365 fix admits arm 3 (own type
// uses T) when:
//   * a re-parsed `ast::PropertyDecl` is attached (`ips->decl` non-null), AND
//   * the property is READ-ONLY (no setter): either expression-bodied
//     (§23.2 — expression bodies have no setter) or block-bodied
//     with only a `get` arm.
// For a property WITH a setter, arm 3 still declines — the WRITE side
// `g.P := v` on a `property P: T` still SEGVs for a reference-typed T
// (per the #2893 follow-up comment recorded in 2945 T5's maintenance block).
// That separate codegen write-path bug is filed as a follow-up; this fix
// scopes narrowly to the read path the #2893 carriage already supports.
//
// ── FAIL-WITHOUT / PASS-WITH (mechanical, verified on commit 053cc36 / 944b398) ─
//
//  T1 — read-only expression-bodied `property P: T => _t;` on `GBase<Pt>`:
//        pre-fix: rejected at sema with #2884 (arm 3 fires); post-fix:
//        monomorphizes, returns 23 (= 11 + 12).
//  T2 — read-only expression-bodied `property P: T => _t;` on `GBase<int>`:
//        pre-fix: same reject; post-fix: monomorphizes, returns 7.
//  T3 — control: a non-T-typed property (`property P: int => _s + 100;`)
//        on the same generic class — the #2893 shape, which the pre-#3365
//        build already accepts. Verifies the new admit didn't change
//        non-arm-3 behavior.
//  T4 — read-only block-bodied with only `get` (`property P: T get => _t;`):
//        pre-fix: same reject; post-fix: monomorphizes, returns 41
//        (= 20 + 21, the second test's Pt instantiation).
//  T5 — CONTROL: a property WITH a setter on a `T`-typed return
//        (`property P: T get => _t; set => _t := value; end;`) stays
//        declined. The 2945 T5 maintenance test pins this; this test
//        keeps that control close to the read-only cases for a single-
//        PR diff view.
//
// (Note on the exit code assertion: each `Main()` returns the computed
// `T`-typed value (`p.X + p.Y` for T1/T4, the int value for T2). That
// value is the program's EXIT code — the test asserts the computed
// value rather than 0, which would be a tautology against an empty
// program body. The harness's `compileImportRun3365` returns -1 on
// compile failure, -2 on dep-toolchain unavailability, otherwise the
// exit code.)
//
// ── RELATED ──────────────────────────────────────────────────────────────
//
// #2884 — original decline; closed for the int-typed case by #2893.
// #2893 — `propertyBodyText` carriage (this fix depends on the importer
//         attaching the re-parsed `ast::PropertyDecl`).
// #2945 T5 — pins the WRITE-side SEGV for a reference-typed T; NOT
//         touched by this fix (the admit is read-only).
// #2583 / #3362 — extension property body carriage; the property-only
//         twin of #2893 (this fix's wire-format half is identical —
//         #2893 already covers the export side).
// #3346 / #3348 — single-module derived-receiver indexer/property
//         monomorphization; pre-condition for this fix in the local
//         form (the codegen's `genericSubst_` setup is the same
//         vehicle).

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

const std::filesystem::path& wtDir3365() {
    return wtScratchDir("wt-3365");
}

struct TempFile3365 {
    std::string path;
    explicit TempFile3365(std::string p) : path(std::move(p)) {}
    ~TempFile3365() { std::remove(path.c_str()); }
};

void writeSource3365(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib + app, link and run, capturing stdout — for the ACCEPT
// controls.
std::pair<int, std::string> compileImportRun3365(const std::string& libSrc,
                                                 const std::string& appSrc,
                                                 const std::string& tag) {
    auto dir = wtDir3365();
    std::string stem = "bz3365_" + tag + "_" + unique_stem("");
    TempFile3365 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3365 aObj((dir / (stem + "_a.o")).string());
    writeSource3365(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return {-2, ""};
    }
    if (!std::ifstream(aObj.path).good()) return {-2, ""};
    TempFile3365 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3365 bExe((dir / (stem + "_b")).string());
    writeSource3365(bSrc.path, appSrc);
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

// Capture stderr — for the write-with-setter control (T5) that
// should still decline with the #2884 diagnostic text.
struct CaptureResult3365 {
    bool depOk = false;
    bool success = false;
    std::string err;
};

CaptureResult3365 compileImportCaptureErr3365(const std::string& libSrc,
                                              const std::string& appSrc,
                                              const std::string& tag) {
    auto dir = wtDir3365();
    std::string stem = "bz3365e_" + tag + "_" + unique_stem("");
    TempFile3365 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3365 aObj((dir / (stem + "_a.o")).string());
    writeSource3365(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return {false, false, ""};
    }
    if (!std::ifstream(aObj.path).good()) return {false, false, ""};

    TempFile3365 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3365 bExe((dir / (stem + "_b")).string());
    writeSource3365(bSrc.path, appSrc);

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

bool has3365(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// A Pt record used for the layout-moving instantiation cases (T2-T4).
// Same shape as the sibling 3346/3348 tests use; kept here so this
// file is self-contained for the §14.1 case.
const char* kPtDef =
    "public record Pt3365 "
    "  public X: int; "
    "  public Y: int; "
    "end Pt3365; ";

}  // namespace

// ===========================================================================
// T1 — REPRO / ACCEPT (the issue's own shape, made to work for the
// expression-bodied read-only case on a layout-moving receiver).
//
// A generic class with an expression-bodied read-only property whose own
// declared type IS the generic param (`property P: T => _t;`), instantiated
// at `Pt3365` (a 2-int record — the layout moves vs. the i32-erased
// placeholder, exactly the test the re-parsed body must monomorphize).
//
// The ctors here are DEFAULT (no-arg) ctors — the issue notes a SEPARATE
// ctor interaction bug (an IMPORTED TYPED ctor failing IR verification
// when monomorphized for `T = Pt`) that is NOT covered by this fix; the
// #3365 fix is the property monomorphization axis, the imported-ctor
// axis is a follow-up. To keep T1-T4 focused on the property path, the
// tests use the default (no-arg) ctor and assign the T-typed storage
// field via an `Init(v: T)` method (the method-monomorphization path
// is closed by #3347). The IMPORTER's typed-ctor monomorphization
// (a #3365 FOLLOW-UP, not part of this fix) is exercised by neither
// T1-T4 nor T5; T5 specifically exercises the declined shape (write
// on a T-typed property) where the ctor is never reached.
//
// Pre-fix: the importer's arm 3 declines with the #2884 diagnostic, and
// `compileImportRun3365` returns -1 (importer compileFile rejected).
// Post-fix: the re-parsed `ast::PropertyDecl` is attached (by
// `parseAndAttachPropertyBody`, the #2893 carriage), arm 3 admits
// because the property is read-only (Expression body has no setter by
// §23.2), the read lowers through the inline-body path with the
// active `genericSubst_` (`T → Pt3365`), and `g.P` returns the
// substituted record — `p.X + p.Y = 11 + 12 = 23`. The Main returns
// 23 (the sum), so the program exit code IS 23 (a positive integer
// is the "ran" sentinel of `compileImportRun3365`).
// ===========================================================================
BEZEL_TEST(xmod_property_body_reparse_3365,
           t1_readonly_expression_typed_pt) {
    auto [code, out] = compileImportRun3365(
        std::string("module Lib3365T1; "
                    "public class GBox3365T1<T> "
                    "  public _t: T; "
                    "  public method Init(v: T) _t := v; end Init; "
                    "  public property P: T => _t; "
                    "end GBox3365T1; ") + kPtDef,
        "module App3365T1; import Lib3365T1; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GBox3365T1<Pt3365>(); "
        "    g.Init(new Pt3365(11, 12)); "
        "    var p: Pt3365 := g.P; "
        "    Console.WriteLine(\"sum=\" + (p.X + p.Y).ToString()); "
        "    return p.X + p.Y; "
        "  end Main; "
        "end C;",
        "t1");
    if (code == -2) return;
    BEZEL_EXPECT_TRUE(code >= 0);  // compile+link succeeded (>=0 means we ran the exe)
    BEZEL_EXPECT_EQ(code, 23);
    BEZEL_EXPECT_TRUE(has3365(out, "sum=23"));
}

// ===========================================================================
// T2 — LAYOUT-IDENTICAL INSTANTIATION: same library as T1, but instantiated
// at `int`. The storage at `_t: T = int` is the same width as the i32
// erased placeholder, so the read works even on the pre-fix ABI — the
// gate (arm 3) is the only thing standing in the way. Verifies the
// read-only admit is the predicate the #2884 diagnostic text
// independently pins.
// ===========================================================================
BEZEL_TEST(xmod_property_body_reparse_3365,
           t2_readonly_expression_typed_int) {
    auto [code, out] = compileImportRun3365(
        "module Lib3365T2; "
        "public class GBox3365T2<T> "
        "  public _t: T; "
        "  public method Init(v: T) _t := v; end Init; "
        "  public property P: T => _t; "
        "end GBox3365T2;",
        "module App3365T2; import Lib3365T2; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new GBox3365T2<int>(); "
        "    g.Init(7); "
        "    var p: int := g.P; "
        "    Console.WriteLine(\"p=\" + p.ToString()); "
        "    return p; "
        "  end Main; "
        "end C;",
        "t2");
    if (code == -2) return;
    BEZEL_EXPECT_TRUE(code >= 0);
    BEZEL_EXPECT_EQ(code, 7);
    BEZEL_EXPECT_TRUE(has3365(out, "p=7"));
}

// ===========================================================================
// T3 — CONTROL (unchanged behavior): a non-T-typed property (`int`) on the
// same generic class — the #2893 shape. Pre-#3365 this already compiled
// and returned 100. Post-#3365 the gate's read-only admit doesn't widen
// to the int-typed case (it was already admitted by arm 3 falling through);
// the propertyBodyText carriage keeps the read path monomorphized per
// instantiation. Both T = int and T = string are tested — the int case
// is layout-identical, the string case is layout-moving.
// ===========================================================================
BEZEL_TEST(xmod_property_body_reparse_3365,
           t3_control_int_typed_int_instantiation) {
    auto [code, out] = compileImportRun3365(
        "module Lib3365T3; "
        "public class GBox3365T3<T> "
        "  public _s: int := 0; "
        "  public property P: int => _s + 100; "
        "end GBox3365T3;",
        "module App3365T3; import Lib3365T3; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new GBox3365T3<int>(); "
        "    Console.WriteLine(\"P=\" + g.P.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t3");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has3365(out, "P=100"));
}

// ===========================================================================
// T4 — READ-ONLY BLOCK-BODIED (the second read-only shape the fix admits):
// `property P: T get => _t;` (Block body with ONLY a `get` arm, no `set`).
// A block-bodied property with no setter is read-only by §23.3, and the
// same re-parsed body lowering applies. Pre-fix: declined (arm 3 fires).
// Post-fix: admitted (arm 3 admits when readOnlyWithReParsedBody = true).
// ===========================================================================
BEZEL_TEST(xmod_property_body_reparse_3365,
           t4_readonly_block_typed_pt) {
    auto [code, out] = compileImportRun3365(
        std::string("module Lib3365T4; "
                    "public class GBox3365T4<T> "
                    "  public _t: T; "
                    "  public method Init(v: T) _t := v; end Init; "
                    "  public property P: T "
                    "    get => _t; "
                    "  end; "
                    "end GBox3365T4; ") + kPtDef,
        "module App3365T4; import Lib3365T4; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GBox3365T4<Pt3365>(); "
                    "    g.Init(new Pt3365(20, 21)); "
        "    var p: Pt3365 := g.P; "
        "    Console.WriteLine(\"sum=\" + (p.X + p.Y).ToString()); "
        "    return p.X + p.Y; "
        "  end Main; "
        "end C;",
        "t4");
    if (code == -2) return;
    BEZEL_EXPECT_TRUE(code >= 0);
    BEZEL_EXPECT_EQ(code, 41);
    BEZEL_EXPECT_TRUE(has3365(out, "sum=41"));
}

// ===========================================================================
// T5 — CONTROL: a property WITH a setter on a `T`-typed return
// (`property P: T get => _t; set => _t := value; end;`) stays declined.
// The 2945 T5 maintenance test pins this exact shape; this test keeps
// that control close to the read-only cases so a single-PR diff view
// shows the admit is properly scoped to read-only properties. Pre-fix
// and post-fix: both decline with the #2884 diagnostic text.
// ===========================================================================
BEZEL_TEST(xmod_property_body_reparse_3365,
           t5_settable_t_typed_still_declined) {
    auto r = compileImportCaptureErr3365(
        "module Lib3365T5; "
        "public class GBox3365T5<T> "
        "  public _t: T; "
        "  public method Init(v: T) _t := v; end Init; "
        "  public property P: T "
        "    get => _t; "
        "    set => do _t := value; end; "
        "  end; "
        "end GBox3365T5;",
        "module App3365T5; import Lib3365T5; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new GBox3365T5<int>(); "
        "    g.Init(7); "
        "    g.P := 8; "
        "    return 0; "
        "  end Main; "
        "end C;",
        "t5");
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(has3365(r.err, "property `P`"));
    BEZEL_EXPECT_TRUE(has3365(r.err, "imported generic type"));
    BEZEL_EXPECT_TRUE(has3365(r.err, "not yet monomorphized"));
}

}  // namespace bezel::module
