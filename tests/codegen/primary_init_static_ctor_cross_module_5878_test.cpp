// fix #5878 (§22.7, v0.334): the primary-initializer lowering emits
// `bz$<T>$initprimary` with `installVtable=false`, and `emitInstanceInit` — which
// is where `emitEnsureStaticInit(t)` lives (LlvmBackend_Setup.cpp:12908) — runs
// ONLY under `installVtable`. §22.7: "A static constructor runs once before the
// type's first use." So when the steps-1+2 call and the vtable install are
// DIFFERENT calls, the static constructor never ran: a silent MISSING SIDE
// EFFECT with no diagnostic — a program prints wrong output rather than failing.
//
// The shape needs all of:
//   1. a `static constructor` on the base (the side effect at risk),
//   2. a PRIMARY-constructor parameter list on the derived (`hasPositionalFields`),
//      so `new T(7, "direct")` binds the primary and `ctorForArgs` finds no
//      explicit-constructor match,
//   3. an explicit constructor, which is what makes the exporter emit
//      `$initprimary` at all (`hasAnyConstructor`, Setup.cpp:13649),
//   4. and the base reached through the primary-base chain from ANOTHER module —
//      the importer calls `primaryInitFn(b)` directly (Setup.cpp:15993) with no
//      preceding `bz$<b>$init`, so nothing on that path calls
//      `emitEnsureStaticInit(b)`.
//
// The static field is `public static readonly Ready: bool` — the shape §22.7's
// own corpus cases use; a mutable `string` static is rejected by §22.12 rule 2.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/run_under_asan.hpp"
#include "support/scratch_dir.hpp"
#include "test_harness.hpp"

namespace bezel::codegen {
namespace {

const std::filesystem::path& wtDir5878() {
    return wtScratchDir("wt-5878-5878");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream f(path);
    f << src;
}

// Capture BOTH stdout and the exit status: a missing `static constructor` print
// is invisible in an exit code alone.
struct RunResult {
    int code = -1;
    std::string out;
};

RunResult runCapture(const std::string& exe) {
    RunResult r;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return r;
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) r.out.append(buf, n);
    int status = pclose(p);
    r.code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return r;
}

struct LibHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> obj;
    bool ok = false;
    std::string objPath;
};

LibHandle buildLib(const std::string& name,
                   const std::filesystem::path& dir,
                   const std::string& libSrc) {
    LibHandle h;
    h.src = std::make_shared<TempFile>((dir / (name + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile>((dir / (name + "_lib.o")).string());
    writeSource(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

struct AppHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> exe;
    bool ok = false;
    std::string exePath;
};

AppHandle buildApp(const std::string& name,
                   const std::filesystem::path& dir,
                   const std::string& libObjPath,
                   const std::string& appSrc) {
    AppHandle r;
    if (libObjPath.empty()) return r;
    r.src = std::make_shared<TempFile>((dir / (name + "_app.bzl")).string());
    r.exe = std::make_shared<TempFile>((dir / (name + "_app")).string());
    writeSource(r.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjPath;
    d.objectPath   = libObjPath;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(r.src->path, r.exe->path, deps);
    r.ok = rb.success;
    r.exePath = r.exe->path;
    return r;
}

// Shared library shape: a base `Base5878` (the type whose static constructor is
// at risk) reached through the primary-base chain from `Derived5878`, which
// carries BOTH a primary-constructor parameter list and an explicit
// constructor.
const char* kLib5878 =
    "module Lib5878; "
    "public class Base5878 "
    "  public static readonly Ready: bool; "
    "  static constructor() "
    "    Console.WriteLine(\"base-static-ran\"); "
    "    Ready := true; "
    "  end constructor; "
    "  public constructor() end constructor; "
    "end Base5878; "
    "public class Derived5878(a: int, b: string): Base5878 "
    "  public Name: string := \"made\"; "
    "  public constructor(x: int) this(x, \"primary\"); end constructor; "
    "end Derived5878; ";

const char* kApp5878 =
    "module App5878;\n"
    "import Lib5878;\n"
    "public class C\n"
    "  public static method Main(): int\n"
    "    var d := new Derived5878(7, \"direct\");\n"
    "    Console.WriteLine(d.Name);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C; ";

// T1 — THE REJECT ROW. §22.7: the static constructor runs once before the type's
// first use. Before the fix, `base-static-ran` is absent from stdout:
// `emitInstanceInit` (the sole `emitEnsureStaticInit` on this path) runs only
// under `installVtable`, and `bz$<Base5878>$initprimary` is emitted with
// `installVtable=false`.
BEZEL_TEST(primary_init_static_ctor_cross_module_5878,
           base_static_ctor_runs_via_initprimary_spec_22_7) {
    const auto& dir = wtDir5878();
    LibHandle libH = buildLib("P5878", dir, kLib5878);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle appH = buildApp("P5878", dir, libH.objPath, kApp5878);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult r = runCapture(appH.exePath);
    BEZEL_EXPECT_EQ(r.code, 0);
    // The static constructor MUST have run before the type's first use.
    BEZEL_EXPECT_TRUE(r.out.find("base-static-ran") != std::string::npos);
    // ...and the field initializers it coexists with (the step-1+2 half of
    // `runStepsOneTwo`) still run — a fix that dropped steps 1+2 would pass the
    // line above and fail this one.
    BEZEL_EXPECT_TRUE(r.out.find("made") != std::string::npos);
}

// T2 — DIRECTIONAL CONTROL. The same library WITHOUT a static constructor.
// What would fail if the fix were deleted: nothing here — this row stays green
// with or without the fix, which is exactly what makes it a control rather than
// a pin. It exists to catch the opposite error: a fix that "passes" T1 by
// breaking the construction path it also touches (e.g. running static init
// where steps 1+2 must not run, or double-running field initializers).
BEZEL_TEST(primary_init_static_ctor_cross_module_5878,
           control_no_static_ctor_still_runs_field_init_spec_22_7) {
    const auto& dir = wtDir5878();
    const std::string libNoStatic =
        "module LibNoStatic; "
        "public class BaseNoStatic "
        "  public constructor() end constructor; "
        "end BaseNoStatic; "
        "public class DerivedNoStatic(a: int, b: string): BaseNoStatic "
        "  public Name: string := \"made\"; "
        "  public constructor(x: int) this(x, \"primary\"); end constructor; "
        "end DerivedNoStatic; ";
    const std::string appSrc =
        "module AppNoStatic;\n"
        "import LibNoStatic;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    var d := new DerivedNoStatic(7, \"direct\");\n"
        "    Console.WriteLine(d.Name);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C; ";
    LibHandle libH = buildLib("P5878Ctl", dir, libNoStatic);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle appH = buildApp("P5878Ctl", dir, libH.objPath, appSrc);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult r = runCapture(appH.exePath);
    BEZEL_EXPECT_EQ(r.code, 0);
    // No static constructor on this base, so nothing may print — and the
    // derived field initializer must still run exactly once.
    BEZEL_EXPECT_TRUE(r.out.find("base-static-ran") == std::string::npos);
    BEZEL_EXPECT_TRUE(r.out.find("made") != std::string::npos);
    // Exactly once: a fix that ran static init twice on this path would print
    // the field initializer's value twice.
    std::size_t first = r.out.find("made");
    BEZEL_EXPECT_TRUE(first != std::string::npos);
    BEZEL_EXPECT_TRUE(r.out.find("made", first + 1) == std::string::npos);
}

// T3 — SINGLE-MODULE PIN. MEASURED fail-without: this row is RED without the fix
// and GREEN with it, so it pins rather than merely controls. It exists because
// the defect is NOT cross-module-only: `emitImplicitBase` is the single
// construction path for a primary-base SUBOBJECT in BOTH the in-module `new`
// site and the imported `$initprimary` path, so a base reached only through an
// inheritance clause never ran its static constructor either way. (My first
// draft of this file asserted the single-module case already worked; measurement
// refuted it — with the base reached as a subobject, stdout was `made` alone.)
//
// The distinguishing control is T2: a DIRECT `new Base()` DOES run the static
// constructor pre-fix, so that shape is unaffected and stays green either way.
BEZEL_TEST(primary_init_static_ctor_cross_module_5878,
           control_single_module_static_ctor_already_runs_spec_22_7) {
    const std::string src =
        "module M; "
        "public class BaseLocal "
        "  public static readonly Ready: bool; "
        "  static constructor() "
        "    Console.WriteLine(\"base-static-ran\"); "
        "    Ready := true; "
        "  end constructor; "
        "  public constructor() end constructor; "
        "end BaseLocal; "
        "public class DerivedLocal(a: int, b: string): BaseLocal "
        "  public Name: string := \"made\"; "
        "  public constructor(x: int) this(x, \"primary\"); end constructor; "
        "end DerivedLocal; "
        "public class C "
        "  public static method Main(): int "
        "    var d := new DerivedLocal(7, \"direct\"); "
        "    Console.WriteLine(d.Name); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    TempFile f((wtDir5878() / "local5878.bzl").string());
    writeSource(f.path, src);
    TempFile exe((wtDir5878() / "local5878").string());
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(f.path, exe.path, {});
    BEZEL_EXPECT_TRUE(r.success);
    if (!r.success) return;
    RunResult rr = runCapture(exe.path);
    BEZEL_EXPECT_EQ(rr.code, 0);
    BEZEL_EXPECT_TRUE(rr.out.find("base-static-ran") != std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("made") != std::string::npos);
}

// T4 — SANITIZER-INSTRUMENTED leg. T1-T3 drive the program through
// `driver.compileFile` + `popen`, which produces an ORDINARY native binary —
// none of them is sanitizer-instrumented, so the suite-wide
// `NATIVE SANITIZER: 0 check(s) skipped` counter says nothing about THIS fix.
// `runUnderAsan` is single-file only, so it cannot express T1's cross-module
// shape; it CAN express T3's, which is the same defect on the same code path
// (`emitImplicitBase`). This row exists so the ASan claim rests on an
// instrumented binary rather than on a counter about someone else's tests.
//
// The extra `emitEnsureStaticInit` runs a type initializer that, before the
// fix, could be SKIPPED — and a skipped init is exactly the shape that leaves
// an `owned` static un-constructed. ASan/LSan here checks that running it
// introduces no leak or use-after-free.
BEZEL_TEST(primary_init_static_ctor_cross_module_5878,
           single_module_static_ctor_under_asan_spec_22_7) {
    const std::string src =
        "module M; "
        "public class BaseAsan "
        "  public static readonly Ready: bool; "
        "  static constructor() "
        "    Console.WriteLine(\"base-static-ran\"); "
        "    Ready := true; "
        "  end constructor; "
        "  public constructor() end constructor; "
        "end BaseAsan; "
        "public class DerivedAsan(a: int, b: string): BaseAsan "
        "  public Name: string := \"made\"; "
        "  public constructor(x: int) this(x, \"primary\"); end constructor; "
        "end DerivedAsan; "
        "public class C "
        "  public static method Main(): int "
        "    var d := new DerivedAsan(7, \"direct\"); "
        "    Console.WriteLine(d.Name); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    // Main returns 0; runUnderAsanValue checks the program's own exit code AND
    // that ASan stayed quiet. It returns -1 ONLY when the ASan runtime object
    // cannot be found (`asanRuntimeObject()` empty), and -2/-3/-4 are the
    // harness's INFRA classes, which it marks FAILED itself (#4504).
    //
    // There is deliberately NO early-return guard on the -1 result here. That
    // shape is a "swallowing guard" (#3682): a missing ASan runtime reads as a
    // SKIP and the row then reports a clean PASS while the leg is off — the row
    // asserts nothing about memory safety in either direction. The family
    // already carries ~40 grandfathered sites; adding a new one is exactly
    // what `check_native_asan_leg_skips.py` refuses, and it is why this file is
    // absent from `scripts/native_asan_leg_skips.grandfather`.
    // (This comment deliberately avoids quoting that guard's source text: the
    // checker matches it textually, so reproducing it here would manufacture
    // the very violation it is warning about.)
    //
    // ASan is available on this host — proven by a planted sentinel that fires
    // `LeakSanitizer: detected memory leaks` — so there is no debt to
    // grandfather and nothing to skip. If the runtime ever does go missing,
    // `a >= 0` below FAILS the row, which is the correct outcome: the oracle
    // cannot be trusted, and silence would be a lie.
    int a = bezel::test::runUnderAsanValue(src, 0, "5878_static_ctor_asan");
    BEZEL_EXPECT_TRUE(a >= 0);  // -1 runtime absent / -2 compile / -3 link / -4 system()
    BEZEL_EXPECT_EQ(a, 0);      // sanitizer report -> 1, non-zero exit -> mismatch
}

}  // namespace
}  // namespace bezel::codegen