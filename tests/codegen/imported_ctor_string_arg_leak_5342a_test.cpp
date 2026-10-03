// Codegen / refcounting — issue #5342 (a): a `+1` string producer passed as
// a CONSTRUCTOR ARGUMENT to an IMPORTED class is never released.
//
// #5322 as filed attributed a 7290 B / 400-allocation leak to an imported
// property read. That prescription was REFUTED by measurement: deleting the
// property read left the leak byte-identical, and a receiver created ONCE
// outside a 200-read loop was clean. What actually leaks is the CONSTRUCTOR
// ARGUMENT. This file covers that half; the fresh-bodied-getter half is (b),
// a separate lane.
//
// THE DEFECT. A `+1` string producer (a concatenation, an interpolation, a
// user-method return) passed DIRECTLY as an argument holds an independent
// rc=1 reference. Constructor formals are BORROWS (§22.7), so the callee
// never takes ownership of that reference. A ctor that only READS the formal
// — or, as here, never mentions it at all — returns with the caller's rc=1
// still outstanding and nothing left to discharge it.
//
// THE ASYMMETRY IS CTOR-VS-METHOD, NOT IMPORTED-VS-LOCAL. Four call sites
// already discharge a `+1` argument producer:
//   - lowerCall's method-call arm          (LlvmBackend_Calls2.cpp:13943)
//   - lowerCall's IMPORTED-method arm      (same predicate, same boundary)
//   - lowerNew's inlined-local-ctor arm     (LlvmBackend_Exprs2.cpp:8899)
//   - lowerNew's declared-ctor-call arm     (LlvmBackend_Exprs2.cpp:10094)
// The fifth — lowerNew's hand-rolled IMPORTED-ctor arm
// (LlvmBackend_Exprs2.cpp, the `allPositional` block) — emitted the
// `bz$<Type>$ctor` call and no release for any argument. That is the missing
// twin this file pins.
//
// Rows:
//   (a1)  impQ — imported ctor `(f: string, n: int)` whose body NEVER
//         mentions `f`. 200 iterations. THE PIN. Leaked 7290 B / 400 allocs
//         pre-fix.
//   (a2)  impV — same import boundary, but the argument is `raw.Trim()`, a
//         DIFFERENT `+1` producer (a user-method return rather than a
//         concatenation). 200 iterations. THE PIN. Leaked 7290 B / 400
//         allocs pre-fix — so the fix is not keyed to one producer shape.
//   (a3)  locO — the same class declared LOCALLY, no import. Must stay clean
//         both legs. Proves the defect is the import boundary, not the ctor
//         shape itself.
//   (a4)  impP — an imported METHOD (not a ctor) ignoring an identical `+1`
//         string argument. Must stay clean both legs. Proves the release
//         added at the ctor boundary did not double-discharge a method
//         argument the method arm already released.
//
// The dependency object MUST outlive the dependent compile —
// `Driver::compileFile` reopens the `.o` to read its `.bezel.meta` section —
// hence the shared_ptr TempFiles.
//
// LEAK ORACLE. Every imported row runs under LeakSanitizer explicitly with
// `exitcode=99`, a value outside every code a correct row returns, and
// asserts on `sawSanitizer` (any "LeakSanitizer:" line in stderr) as well as
// on stdout. A leak therefore can never be read as a pass. Under the `--full`
// sweep `compileFileForTest` additionally instruments the generated code;
// this leg is deliberately self-contained so the LSan claim holds in a plain
// lane run too.
//
// Spec §4.6 "`string`" / §28.9 "Runtime-managed types" (fresh-class disposal) /
// §23.2 "Deferred visibility" (the import boundary) / §14.1 "Owner
// substitution" / §2.5 "Library import" — spec revision v0.313.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/build_paths.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {

const int kIters = 200;

const std::filesystem::path& wtDir5342a() {
    return bezel_test::wtScratchDir("wt-5342a-codegen");
}

struct TempFile {
    std::string path;
    explicit TempFile(std::string p) : path(std::move(p)) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Run {
    std::shared_ptr<TempFile> libSrc, libObj, appSrc, appExe;
    std::string out;
    int code = -1;
    bool built = false;
    bool sawSanitizer = false;
};

// Compile `app` and run the result under LeakSanitizer. `sawSanitizer` is
// true if stderr carried ANY sanitizer report, so a leak is a hard failure of
// the row rather than something the exit code could absorb. An empty `lib`
// compiles the app as a SINGLE module (the local twin) — `deps` is empty and
// no library object is built at all.
Run runProgram(const std::string& lib, const std::string& app) {
    Run r;
    auto dir = wtDir5342a();
    const std::string stem = bezel_test::unique_stem("bezel_5342a");
    r.appSrc = std::make_shared<TempFile>((dir / (stem + "_app.bzl")).string());
    r.appExe = std::make_shared<TempFile>((dir / (stem + "_app")).string());
    writeSource(r.appSrc->path, app);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        if (!lib.empty()) {
            // The dependency object MUST outlive the dependent compile —
            // `Driver::compileFile` reopens it to read its `.bezel.meta`.
            r.libSrc = std::make_shared<TempFile>(
                (dir / (stem + "_lib.bzl")).string());
            r.libObj = std::make_shared<TempFile>(
                (dir / (stem + "_lib.o")).string());
            writeSource(r.libSrc->path, lib);
            bezel::CompileResult lr =
                driver.compileObject(r.libSrc->path, r.libObj->path);
            if (!lr.success) return r;
            bezel::Dependency d;
            d.metadataPath = r.libObj->path;
            d.objectPath = r.libObj->path;
            deps.push_back(d);
        }
        // A LEAK REGRESSION TEST MUST INSTRUMENT ITS OWN GENERATED CODE.
        // `compileFileForTest` consults `BEZEL_AOT_FORCE_SANITIZE` (the
        // `--full` sweep) and is byte-for-byte inert without it, so a plain
        // lane run would compile an UNINSTRUMENTED program: LSan would then
        // report nothing and this file's oracle would be vacuously green on
        // the exact code it exists to reject. `withForcedSanitize` only ever
        // turns the bit ON, so setting it here keeps the plain run honest and
        // leaves the `--full` sweep unchanged (the established per-file
        // remedy — tests/module/xmod_object_tostring_producer_residual_2867
        // _test.cpp:101).
        //
        // The library is NOT compiled with it: `Driver::compileObject` forces
        // `sanitizeAddress` off by design (it does not link), and it does not
        // need it — linking the ASan runtime into the EXECUTABLE interposes
        // `malloc` process-wide, so the dependency's allocations are covered
        // and only redzone instrumentation is importer-local
        // (driver_run_support.hpp, "WHY compileObject IS NOT WRAPPED").
        bezel::codegen::CodegenOptions opts;
        opts.sanitizeAddress = true;
        bezel::CompileResult ar = bezel::test::compileFileForTest(
            driver, r.appSrc->path, r.appExe->path, deps, opts);
        if (!ar.success) return r;
    }
    r.built = true;
    const std::string cmd =
        "ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:exitcode=99 " +
        r.appExe->path + " 2>" + r.appExe->path + ".err";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return r;
    char buf[512];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
        r.out.append(buf, n);
    int status = pclose(p);
    r.code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    {
        std::ifstream err(r.appExe->path + ".err");
        std::string text((std::istreambuf_iterator<char>(err)),
                         std::istreambuf_iterator<char>());
        r.sawSanitizer =
            text.find("AddressSanitizer:") != std::string::npos ||
            text.find("LeakSanitizer:") != std::string::npos;
    }
    std::remove((r.appExe->path + ".err").c_str());
    return r;
}

// The loop prints one line per iteration, so the assertion is on stdout —
// never on an exit code, which a leak could not be distinguished through on
// its own.
std::string wantPrinted(int iters) {
    std::string w;
    for (int i = 0; i < iters; ++i) w += "L" + std::to_string(i) + "\n";
    return w;
}

// ── the shared dependency ──────────────────────────────────────────────────
// `Q` is the pin's library: a constructor that IGNORES its string formal
// entirely (assigns only `_n`), so nothing in the callee can ever take
// ownership of the argument. `Take` is the control's method, which likewise
// ignores its string formal.
const char* kLibQ =
    "module Lib5342a; "
    "public class Q "
    "  private _n: int; "
    "  public constructor(f: string, n: int) _n := n; end constructor; "
    "  public constructor() _n := 1; end constructor; "
    "  public method Take(f: string): int _n := 2; return 1; end Take; "
    "end Q; ";

// (a1) The argument is a CONCATENATION — the canonical `+1` producer. The
// ctor body never mentions `f`, so ownership is never taken on the far side.
std::string appQ(int iters) {
    return "module App5342a; "
           "import Lib5342a; "
           "public class Program "
           "  public static method Main(): int "
           "    for var i in 0.." "<" + std::to_string(iters) + " do "
           "      using var q := new Q(\"o\" + i.ToString(), i); "
           "      Console.WriteLine(\"L\" + i.ToString()); "
           "    end; "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// (a2) The argument is `raw.Trim()` — a USER-METHOD RETURN, a structurally
// different `+1` producer from (a1)'s concatenation. A fix keyed to the
// concatenation's shape rather than to `isPlusOneStringProducer` would leave
// this row leaking.
std::string appV(int iters) {
    return "module App5342a; "
           "import Lib5342a; "
           "public class Program "
           "  public static method Main(): int "
           "    for var i in 0.." "<" + std::to_string(iters) + " do "
           "      var raw := \"  x\" + i.ToString() + \"  \"; "
           "      using var q := new Q(raw.Trim(), i); "
           "      Console.WriteLine(\"L\" + i.ToString()); "
           "    end; "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// (a4) The control's imported METHOD takes the identical `+1` concatenation
// argument and likewise ignores it. `lowerCall` already released this, so the
// row pins that the ctor-boundary release did not perturb it.
std::string appP(int iters) {
    return "module App5342a; "
           "import Lib5342a; "
           "public class Program "
           "  public static method Main(): int "
           "    var t := 0; "
           "    for var i in 0.." "<" + std::to_string(iters) + " do "
           "      using var q := new Q(); "
           "      t := t + q.Take(\"o\" + i.ToString()); "
           "      Console.WriteLine(\"L\" + i.ToString()); "
           "    end; "
           "    Console.WriteLine(t.ToString()); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// (a1) THE PIN — imported ctor ignoring a `+1` concatenation argument.
// FAIL-WITHOUT (MEASURED on r23-c28 = 67bab85c, `bezelc --sanitize=address`
// + LeakSanitizer, 200 iterations):
//   Direct leak of 6400 byte(s) in 200 object(s)
//   Indirect leak of 890 byte(s) in 200 object(s)
//   SUMMARY: AddressSanitizer: 7290 byte(s) leaked in 400 allocation(s)
// ═══════════════════════════════════════════════════════════════════════════
BEZEL_TEST(imported_ctor_string_arg_leak_5342a,
           pin_imported_ctor_ignoring_concat_arg_released_spec_2_5_4_6_23_2_28_9) {
    Run r = runProgram(kLibQ, appQ(kIters));
    BEZEL_EXPECT_TRUE(r.built);
    if (!r.built) return;
    BEZEL_EXPECT_FALSE(r.sawSanitizer);
    BEZEL_EXPECT_EQ(r.code, 0);
    BEZEL_EXPECT_EQ(r.out, wantPrinted(kIters));
}

// ═══════════════════════════════════════════════════════════════════════════
// (a2) THE PIN — the same boundary with a DIFFERENT `+1` producer.
// FAIL-WITHOUT (MEASURED on r23-c28 = 67bab85c, same leg):
//   SUMMARY: AddressSanitizer: 7290 byte(s) leaked in 400 allocation(s)
// ═══════════════════════════════════════════════════════════════════════════
BEZEL_TEST(imported_ctor_string_arg_leak_5342a,
           pin_imported_ctor_ignoring_trim_arg_released_spec_2_5_4_6_23_2_28_9) {
    Run r = runProgram(kLibQ, appV(kIters));
    BEZEL_EXPECT_TRUE(r.built);
    if (!r.built) return;
    BEZEL_EXPECT_FALSE(r.sawSanitizer);
    BEZEL_EXPECT_EQ(r.code, 0);
    BEZEL_EXPECT_EQ(r.out, wantPrinted(kIters));
}

// ═══════════════════════════════════════════════════════════════════════════
// (a3) CONTROL — the same class declared LOCALLY, no import. CLEAN pre-fix and
// post-fix (MEASURED at r23-c28 = 67bab85c: exit 0, no sanitizer output). If
// this row ever leaked, (a1)'s failure would be attributable to the ctor shape
// rather than to the import boundary.
// ═══════════════════════════════════════════════════════════════════════════
BEZEL_TEST(imported_ctor_string_arg_leak_5342a,
           control_local_ctor_ignoring_concat_arg_clean_spec_2_5_4_6_23_2_28_9) {
    Run r = runProgram(
        /*lib=*/"",  // no import: the local twin is a single module
        "module App5342a; "
        "public class Q "
        "  private _n: int; "
        "  public constructor(f: string, n: int) _n := n; end constructor; "
        "end Q; "
        "public class Program "
        "  public static method Main(): int "
        "    for var i in 0.." "<" + std::to_string(kIters) + " do "
        "      using var q := new Q(\"o\" + i.ToString(), i); "
        "      Console.WriteLine(\"L\" + i.ToString()); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.built);
    if (!r.built) return;
    BEZEL_EXPECT_FALSE(r.sawSanitizer);
    BEZEL_EXPECT_EQ(r.code, 0);
    BEZEL_EXPECT_EQ(r.out, wantPrinted(kIters));
}

// ═══════════════════════════════════════════════════════════════════════════
// (a4) CONTROL — the imported METHOD twin, taking the identical `+1`
// concatenation argument and likewise ignoring it. CLEAN pre-fix and post-fix
// (MEASURED at r23-c28 = 67bab85c: exit 0, no sanitizer output). This row is
// what makes (a1)'s failure attributable to CTOR-VS-METHOD rather than to
// the import boundary alone, and it pins that the ctor-boundary release added
// here did not double-discharge a method argument `lowerCall` already
// released.
// ═══════════════════════════════════════════════════════════════════════════
BEZEL_TEST(imported_ctor_string_arg_leak_5342a,
           control_imported_method_ignoring_concat_arg_clean_spec_2_5_4_6_23_2_28_9) {
    Run r = runProgram(kLibQ, appP(kIters));
    BEZEL_EXPECT_TRUE(r.built);
    if (!r.built) return;
    BEZEL_EXPECT_FALSE(r.sawSanitizer);
    BEZEL_EXPECT_EQ(r.code, 0);
    // `Take` returns 1 on every iteration, so the trailing total is `kIters`.
    BEZEL_EXPECT_EQ(r.out, wantPrinted(kIters) + std::to_string(kIters) + "\n");
}
