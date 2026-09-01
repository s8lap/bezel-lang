// Spec-derived regression test for issue #3625 — the CODEGEN half. An
// IMPORTED class with BOTH a primary constructor AND an explicit convenience
// constructor MUST construct correctly when called via either entry point:
//   - the convenience ctor (the (1) shape: `new WithConv(6)`),
//   - the primary ctor directly (the (2) shape: `new WithConv(7, "direct")`).
//
// spec §22.10 "Primary constructors" (v0.286): "A class may declare its
// primary constructor's parameters in the class header." A `new` site whose
// argument list matches the primary ctor's positional parameter list is a
// valid construction entry point (§22.5/§22.10), and the importer-side
// codegen MUST populate the primary-parameter fields from the call's
// arguments (§22.8 step 0, "Primary-constructor parameters ... are captured
// into their compiler-generated private fields" — the importer observes the
// same surface the exporter exposes per §2.8.3 rule 4).
//
// spec §2.8.2 clause 1 / §2.8.3 rule 4 (v0.286): the importer observes the
// same construction surface the exporter exposes. A default-initialized
// imported instance — zero values for fields the call bound — is a silent
// §2.8.3 violation: the importer lowered a construction that did not run the
// constructor body, returning a value the same exporter would never produce
// for the character-identical call in a single-module compilation.
//
// spec §22.5 "Optional parameters and named arguments" (v0.286): a call site
// with the arity of the primary ctor's positional parameter list is a §22.5
// accept. The sema-side half of #3625 (W3-L1's `narrow #3625`) re-admitted
// this call; the codegen-side half (this fix) restores the §22.5 accept
// semantics to the runtime value.
//
// ── WHAT THE DEFECT WAS (measured on `ee4f935` pre-fix) ─────────────────────
//
// `lowerNew`'s imported non-generic arm at line 5930 ran §22.8 step 0
// (`storePrimaryCtorPositionalArgs`) only for the no-explicit-ctor case:
// gated on `!hasAnyConstructor(t)`. For an imported class with BOTH a
// primary ctor AND an explicit convenience ctor, the gate SKIPPED step 0 —
// the comment claimed "would be a second, unasked-for write" relative to
// the convenience ctor's chain call. Two consequences on the (2) shape:
//   - `new WithConv(7, "direct")` against the imported class: sema accepted
//     (via the W3-L1 fall-through to `checkPositionalNewArgsFromSymbols`),
//     `ctorForArgs(t, ne.args)` returned null (the primary has no
//     ConstructorSymbol — only the convenience ctor does, and its arity is
//     1), the explicit-ctor call block emitted the #2658 "no constructor
//     call could be lowered" warning, and the instance kept its calloc-zero
//     field values. `Show()` printed `0\n` instead of `7\ndirect`.
//   - The (1) shape happened to work: the convenience ctor's compiled body
//     (`bz$<XLib.WithConv>$ctor`) inlined the chain call `this(x, "conv")`,
//     which in turn inlined the primary ctor's body (the same
//     `storePrimaryCtorPositionalArgs` helper) — so step 0 ran INSIDE the
//     convenience ctor's body, populating fields `a` and `b`. The
//     importer-side step-0 skip was wasted but invisible.
//
// ── THE FIX SHAPE ──────────────────────────────────────────────────────────
//
// Remove the `!hasAnyConstructor(t)` gate in the imported non-generic arm,
// so `storePrimaryCtorPositionalArgs` runs for has-explicit-ctor imported
// classes too. The (1) shape is unchanged in observed behavior — the
// convenience ctor's chain still writes the same primary-field values
// AFTER the importer-side step 0, so the importer-side write is wasted but
// never incorrect (the chain overwrites with the same values, exactly the
// posture the LOCAL arm at line 6102 takes). The (2) shape is now correct:
// the importer-side step 0 is the ONLY place the primary ctor's body
// runs, and removing the gate is what makes it run.
//
// The fix's loud→quiet transition (the #2658 warning) was a false positive
// for the (2) shape post-fix (the warning's "every field it assigns keeps
// its default" was FALSE — step 0 had populated them). The warning is now
// suppressed when the importer-side step 0 ran: the precise predicate is
// `isImported(t) && t->hasPositionalFields && !cs (ctorForArgs returned
// null, meaning no explicit ctor matched the call) && !ne.args.empty()`
// — the (2) shape exactly.
//
// ── ORACLE CHOICE ──────────────────────────────────────────────────────────
//
// AOT execution (compile + link + run) is the oracle for BOTH (a) and (b)
// — the JIT pass skips ownership/definite-assignment verification and has
// no module boundary, so an in-process JIT test would prove nothing about
// the imported-side step-0 lowering the fix touches. `runAotChecked` would
// also work; `runAot` is what the cross-module pins use. The print-based
// assertions (`out.find("7") != npos && out.find("direct") != npos` for
// the (2) shape; `out.find("6") != npos && out.find("conv") != npos` for
// the (1) shape) avoid the §28.11 disposal-trap the AOT exit-truncation
// hazard §10 of the lane rules names (output > 255 moves to stdoutText,
// not the exit code).
//
// ── FAIL-WITHOUT / PASS-WITH ──────────────────────────────────────────────
//
// (a) `new WithConv(6)` pre-fix: PASSES (the (1) shape's chain inlined
//     step 0 in the convenience ctor's body — pre-existing behaviour,
//     unchanged by the fix). Post-fix: still PASSES, no warning. Pinning
//     it pins that the fix did not regress the (1) shape.
// (b) `new WithConv(7, "direct")` pre-fix: emits "internal: no constructor
//     call could be lowered" warning AND prints `0\n` (calloc-zero `a`
//     and empty `b`). Post-fix: compiles clean (no warning) AND prints
//     `7\ndirect` (step 0 stored the call's args into primary fields).
//     This is the load-bearing row.

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::codegen {
namespace {

// Per-process unique working dir under the shared scratch base (same
// rationale as the sibling imported-ctor tests: /tmp tmpfs races between
// fork'd shards would silently destroy the lib.o the app build needs to
// link against).
const std::filesystem::path& wtDir3625() {
    return wtScratchDir("wt-3625-codegen");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Run a binary and return {exitCode, combined-stdout-and-stderr}. Used by
// the AOT-execution oracle — `Driver`'s in-process `CompileResult` carries
// only `success` and `errorCount`, so to assert the §22.10 runtime value
// we run the linked binary and capture its stdout (the `using` `var` `s`/
// `t` calls `Show`, which `Console.WriteLine`s the field values).
struct RunResult { int code; std::string out; };

RunResult runCapture(const std::string& exe, const std::string& args = "") {
    std::string out;
    FILE* p = popen((exe + (args.empty() ? "" : " " + args) +
                     " 2>&1").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

// Build the shared library object once per test, return its path. The
// object is consumed by every dependent (driver or CLI) compile.
//
// Lifecycle note: the `TempFile` for the .o MUST outlive any subsequent
// `buildApp` call — the importer-side `compileFile` reopens the .o to
// read its `.bezel.meta` section, and a deleted .o shows up as
// `bezelc: error: cannot load module metadata from ...`. Keeping the
// returned `std::shared_ptr` alive in the caller (alongside the matching
// app TempFiles) holds the file open for the duration of the test.
struct LibHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle buildLib(const std::filesystem::path& dir,
                   const std::string& libSrc) {
    LibHandle h;
    std::string stem = unique_stem("bezel_3625lib_");
    h.src = std::make_shared<TempFile>((dir / (stem + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile>((dir / (stem + "_lib.o")).string());
    writeSource(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

// In-process importer-side compile (uses `Driver::compileFile`). Returns
// the linked executable path so the test can run it and observe the §22.10
// runtime value.
struct InProcessApp {
    bool ok = false;
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> exe;
    std::string exePath;
    // Captured compile diagnostics — both the streamily of `Driver`
    // (compileFile discards this on success per `aot_support.hpp:876`) and
    // the subprocess path's stderr (the (a) shape's path writes a
    // `#2658` warning that we want to assert against).
    std::string stderrText;
};

InProcessApp buildApp(const std::filesystem::path& dir,
                      const std::string& libObjPath,
                      const std::string& appSrc) {
    InProcessApp r;
    if (libObjPath.empty()) return r;
    std::string stem = unique_stem("bezel_3625app_");
    r.src = std::make_shared<TempFile>((dir / (stem + "_app.bzl")).string());
    r.exe = std::make_shared<TempFile>((dir / (stem + "_app")).string());
    writeSource(r.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d; d.metadataPath = libObjPath; d.objectPath = libObjPath;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(r.src->path, r.exe->path, deps);
    r.ok = rb.success;
    r.exePath = r.exe->path;
    // `Driver::CompileResult` carries `success` + `errorCount` only — for
    // the warning-text check on the (b) shape we shell out to `bezelc` to
    // capture its stderr. That is delegated to the test (run the binary
    // path captures stdout; the warning predicate is computed from the
    // `Driver::compileFile` result combined with the subprocess run).
    return r;
}

// Re-run the compile via the `bezelc` CLI so we can capture its stderr —
// the in-process `Driver::compileFile` discards diagnostic text on success
// (`aot_support.hpp:876`, exactly the lane-rule §3 vacuous-test trap).
// Returns the CLI's stderr verbatim.
std::string compileViaCli(const std::string& libObjPath,
                          const std::string& appSrcPath) {
    std::string cmd = "/home/stepan/w/code/bezel-lang/.claude/lane-pool/lane-10/build/bezelc "
                      "--import " + libObjPath +
                      " -o /tmp/bezel_3625_cli_app " + appSrcPath + " 2>&1";
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    (void)status;
    std::remove("/tmp/bezel_3625_cli_app");
    return out;
}

}  // namespace

// ── (a) (1) SHAPE — convenience ctor call — anti-regression pin ────────────
//
// `new WithConv(6)` against the imported
// `class WithConv(a: int, b: string) + constructor(x: int) this(x, "conv")`.
// Pre-fix and post-fix BOTH succeed (the (1) shape's chain inlined step 0
// in the convenience ctor's body, populating fields `a` and `b`). The fix
// must NOT regress this shape — it is the load-bearing "did you touch
// something you shouldn't have" pin.
BEZEL_TEST(xmod_imported_primary_with_convenience_ctor_body_lowering_3625,
           convenience_call_path_prints_value_spec_22_10) {
    auto dir = wtDir3625();
    const std::string lib =
        "module XLib3625a; "
        "public class WithConv(a: int, b: string) "
        "  public constructor(x: int) "
        "    this(x, \"conv\"); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(a.ToString()); "
        "    Console.WriteLine(b); "
        "  end Show; "
        "end WithConv; ";
    const std::string app =
        "module XMain3625a; "
        "import XLib3625a; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new WithConv(6); "
        "    s.Show(); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(out.find("6") != std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("conv") != std::string::npos);
}

// ── (b) (2) SHAPE — primary ctor direct call — HEADLINE ───────────────────
//
// `new WithConv(7, "direct")` against the SAME imported class. Sema binds
// to the primary directly via the W3-L1 §22.5/§22.10 fall-through, so
// `ctorForArgs(t, ne.args)` returns null and the explicit-ctor call block
// has no `bz$<XLib.WithConv>$ctor` to invoke. Pre-fix: step 0 was SKIPPED
// (the `!hasAnyConstructor(t)` gate) AND the #2658 warning fired; the
// instance kept its calloc-zero field values; `Show()` printed `0\n`.
// Post-fix: step 0 ran (the gate is gone); the warning is suppressed
// (the importer-side predicate fires); `Show()` prints `7\ndirect`.
//
// This is the load-bearing row: a regression that re-introduces the gate
// would print `0\n` instead of `7\ndirect`, distinguishable from any
// correct result.
BEZEL_TEST(xmod_imported_primary_with_convenience_ctor_body_lowering_3625,
           primary_call_path_prints_value_spec_22_10) {
    auto dir = wtDir3625();
    const std::string lib =
        "module XLib3625b; "
        "public class WithConv(a: int, b: string) "
        "  public constructor(x: int) "
        "    this(x, \"conv\"); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(a.ToString()); "
        "    Console.WriteLine(b); "
        "  end Show; "
        "end WithConv; ";
    const std::string app =
        "module XMain3625b; "
        "import XLib3625b; "
        "class C "
        "  public static method Main(): int "
        "    using var t := new WithConv(7, \"direct\"); "
        "    t.Show(); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    // The §22.10 runtime value — a=7, b="direct" — is the oracle. Pre-fix
    // printed "0\n" because the calloc-zero default left both fields empty.
    BEZEL_EXPECT_TRUE(out.find("7") != std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("direct") != std::string::npos);
}

// ── (b') NO-WARNING — (b)'s compile does NOT emit the #2658 warning ────────
//
// Pre-fix emitted "internal: no constructor call could be lowered for this
// imported type ..." (Severity::Warning) on the (2) shape. Post-fix the
// warning is suppressed by the new `importerStep0Populated` predicate. We
// assert the predicate's effect via a subprocess compile (the in-process
// `Driver::compileFile` discards diagnostics on success — see
// `aot_support.hpp:876` and the lane-rule §3 vacuous-test trap).
BEZEL_TEST(xmod_imported_primary_with_convenience_ctor_body_lowering_3625,
           primary_call_path_emits_no_warning_spec_22_10) {
    auto dir = wtDir3625();
    const std::string lib =
        "module XLib3625c; "
        "public class WithConv(a: int, b: string) "
        "  public constructor(x: int) "
        "    this(x, \"conv\"); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(a.ToString()); "
        "    Console.WriteLine(b); "
        "  end Show; "
        "end WithConv; ";
    const std::string app =
        "module XMain3625c; "
        "import XLib3625c; "
        "class C "
        "  public static method Main(): int "
        "    using var t := new WithConv(7, \"direct\"); "
        "    t.Show(); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    // The (b') test needs an APP source — the call site is on the consumer
    // side, not the library side. The first attempt of this test passed
    // `libH.src->path` by mistake (the library contains NO `new WithConv`
    // call), so the CLI compile succeeded with no warning, and the
    // absence-of-warning assertion passed vacuously (lane-rule §3).
    // Re-running through `buildApp` first forces a *real* source file at
    // a known path; we then capture that path by inspecting the most
    // recently created file in `dir`. (The harness doesn't expose the
    // interpreted stems; matching the on-disk files is the cheaper fix
    // than teaching `InProcessApp` a return-stem path.)
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    std::string stderrText = compileViaCli(libH.objPath, r.src->path);
    // Post-fix: no warning. Pre-fix: the #2658 warning fired here.
    BEZEL_EXPECT_TRUE(stderrText.find("no constructor call could be lowered")
                      == std::string::npos);
}

}  // namespace bezel::codegen
