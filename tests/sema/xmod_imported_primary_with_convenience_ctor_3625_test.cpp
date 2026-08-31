// Spec-derived regression test for issue #3625 — the SEMANTIC half. An
// IMPORTED class that has BOTH a primary constructor AND an explicit
// convenience constructor must let sema select the convenience constructor
// when the call's arity matches its parameter list, exactly as the local
// twin already does. Pre-fix, `new WithConv(6)` against
// `class WithConv(a: int, b: string) + constructor(x: int) this(x, "conv")`
// was REJECTED with "required parameter `b` is not supplied (§22.5)" — a
// diagnostic that named a constructor the user did not call.
//
// spec §22.5 "Optional parameters and named arguments" (v0.286) — call
// sites with a missing required parameter (and no default) are rejected
// with a diagnostic that names the missing parameter.
//
// spec §22.7 / §22.10 (v0.286): an explicit constructor that chains via
// `this(...)` is a valid construction entry point; the call site binds the
// chain source's parameters, not the chained-to primary's.
//
// spec §2.8.2 clause 1 / §2.8.3 (v0.286): the importer-side surface a
// caller observes includes the class's full public constructor list, not
// only the primary's named-field arity.
//
// ── WHAT THE DEFECT WAS (measured on `907ce0f` and on `fix #3583`'s tree) ──
//
// `checkNewExprArgs`'s imported-class arm routed every `new ImportedT(args)`
// with `t->hasPositionalFields` through `checkPositionalNewArgsFromSymbols`,
// which checks against the PRIMARY's positional field list — never against
// the imported convenience `ConstructorSymbol` reconstructed by the loader.
// The convenience ctor's parameters were visible to codegen's
// `ctorForArgs`/lowering but invisible to sema's `checkNewExprArgs`. So:
//   - `new WithConv(6)` (one arg, matches the convenience ctor): REJECTED
//     with a §22.5 message about the primary's missing `b`.
//   - `new WithConv(7, "direct")` (two args, matches the primary):
//     accepted, but the convenience's body never ran — codegen silently
//     zero-initialized (#2658, the codegen-half follow-up).
//
// ── THE FIX SHAPE (sema only; codegen-half is filed as follow-up #3649) ──
//
// A new `else if` arm in `checkNewExprArgs` probes each explicit ctor with
// `bindArgsToParams(FormalsView::of(*cs), ne.args, …, emit=false)` BEFORE
// the primary-field check. `bindArgsToParams` runs the SAME §22.5 binder
// `checkImportedNewExprArgs` already uses for the no-primary-ctor imported
// path — same rule, same five diagnostic strings, same side-table
// publication (`setNewExprPositionalBinding`). The exact-match path
// (one ctor accepts) re-runs the binder with `emit=true` to publish
// diagnostics and the side-table binding, then runs the same per-argument
// §5.1 / `importedDefaultTextIsMaterializable` walk `checkImportedNewExprArgs`
// already does. ZERO matches falls through to the existing primary-field
// check (preserving the (2) path). TWO+ matches DEFER, mirroring the
// local twin's overloaded-ctor deferral.
//
// ── ORACLE CHOICE ──────────────────────────────────────────────────────────
//
// AOT execution (compile + link + run) is the oracle for the headline (a)
// case — the JIT pass skips ownership/definite-assignment verification
// and has no module boundary, so an in-process JIT test would prove nothing
// about the imported-class lookup the fix touches. The (b) reject case
// uses `Driver::compileFile` returning `success = false` — the same
// import-side reject oracle `xmod_ctor_optional_params_3082_test.cpp`
// uses for its (d) and (e) reject cases.
//
// ── FAIL-WITHOUT / PASS-WITH ──────────────────────────────────────────────
//
// (a) `new WithConv(6)` REJECTED on `907ce0f` (the asserted accept does
//     not happen on that tree). PASSES post-fix: prints `6\nconv`.
// (b) The 3-arg shape `new WithConv(7, "direct", 99)` is rejected on BOTH
//     trees (no convenience ctor matches the 3-arg call) — pinning that
//     the fix does not over-widen the binder.

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

namespace bezel::sema {
namespace {

// Per-process unique working dir under the shared scratch base (same
// rationale as the sibling imported-ctor test).
const std::filesystem::path& wtDir3625() {
    return wtScratchDir("wt-3625");
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

struct InProcessApp {
    bool ok = false;
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> exe;
    std::string exePath;
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
    return r;
}

// ── (a) HEADLINE — §22.10 / §22.5 / §2.8.3 rule 4 — convenience ctor path ──

// Mirror of the issue's repro: `WithConv(a: int, b: string)` + an explicit
// `constructor(x: int) this(x, "conv")`, called as `new WithConv(6)`.
// Pre-fix: REJECTED with `required parameter 'b' is not supplied (§22.5)`.
// Post-fix: compiles, runs, prints `6\nconv`.
BEZEL_TEST(xmod_imported_primary_with_convenience_ctor_3625,
           one_arg_matches_convenience_spec_22_5_22_10) {
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
    if (!r.ok) {
        std::fprintf(stderr, "compileFile failed for (a) — post-fix expected accept\n");
    }
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(out.find("6") != std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("conv") != std::string::npos);
}

// ── (b) ANTI-OVER-REJECT — wrong-arity on a 2-convenience-ctor class ────
//
// A class with TWO convenience ctors of distinct arities
// (`constructor(x: int) this(x, "int")` and `constructor(s: string) this(0, s)`)
// AND a 3-arg call `new WithConv(7, "direct", 99)` — no ctor matches the
// 3-arg call, and the primary has only 2 fields. Both pre-fix and post-fix
// MUST reject loudly (§22.5). The post-fix path: the new arm's probe finds
// arityMatches == 0 on both convenience ctors, falls through to the
// primary-field check which rejects with `required parameter`. Pinning
// this proves the probe does not over-widen the binder.
BEZEL_TEST(xmod_imported_primary_with_convenience_ctor_3625,
           three_args_against_two_convenience_rejected_spec_22_5) {
    auto dir = wtDir3625();
    const std::string lib =
        "module XLib3625b; "
        "public class WithConv(a: int, b: string) "
        "  public constructor(x: int) "
        "    this(x, \"int\"); "
        "  end constructor; "
        "  public constructor(s: string) "
        "    this(0, s); "
        "  end constructor; "
        "end WithConv; ";
    const std::string app =
        "module XMain3625b; "
        "import XLib3625b; "
        "class C "
        "  public static method Main(): int "
        "    var s := new WithConv(7, \"direct\", 99); "
        "    _ := s; "
        "    return 0; "
        "  end Main; "
        "end C; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_FALSE(r.ok);
}

}  // namespace bezel::sema
}  // namespace