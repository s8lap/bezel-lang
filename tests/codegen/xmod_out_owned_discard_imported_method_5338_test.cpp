// Regression test for issue #5338 — `out _` against an IMPORTED `out owned`
// formal does not lower (degraded to zero value, callee never called).
//
// Spec §22.6 (v0.313): "`out _` is admitted against an `out owned T` formal."
// §22.4 for imported callees is the same call form.
//
// WHAT WAS BROKEN. The imported-method argument-lowering loop in
// `LlvmBackend_Calls2.cpp` bailed when `a.value` was null — which is the
// case for every discard (`a.isDiscard` is true, `a.value` is nullptr by
// design, Node.hpp `CallArgument::value`). The `!a.value` guard abandoned the
// call (returned nullptr), which degraded to zero-initialization of the
// surrounding binding and a #2434 `internal:` warning. The callee was never
// called.
//
// THE FIX. Mirrors `buildOrderedArgs`'s discard handling (LlvmBackend_Assign
// :14452-14538): when `a.isDiscard` is true and the formal is `out` or
// `out owned`, allocate a scratch slot of the formal's type and use it for the
// call. For `out owned` class/interface, also null-init the slot and register
// it for post-call disposal (§28.3).
//
// ── ORACLE CHOICE ──────────────────────────────────────────────────────────
//
// AOT execution (compile + link + run) is the strict oracle: the cross-module
// shape is invisible to the JIT pass (no module boundary to cross) and the JIT
// path skips `verifyModule`. `runAot` is single-module, so the multi-module
// cases use `Driver::compileObject` + `compileFile` with a `Dependency` (same
// shape as #2845 / #3082 / #3228).

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"


namespace bezel::codegen {
namespace {

const std::filesystem::path& wtDir5338() {
    return wtScratchDir("wt-5338");
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

RunResult runCapture(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
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
    std::string stem = unique_stem("bezel_5338lib_");
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
    std::string stem = unique_stem("bezel_5338app_");
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

}  // namespace

// ── (a) HEADLINE — `out _` against imported `out owned Item?` static method ──
//
// Pre-fix: compiles with #2434 "degraded to a zero-initialized value" warning,
// `ok` is false, the callee is never called, `Main` returns 0.
// Post-fix: call lowers and runs, `ok` is true, stdout is "ok".
BEZEL_TEST(xmod_out_owned_discard_imported_method_5338,
           out_owned_discard_on_imported_static_method_spec_22_6) {
    auto dir = wtDir5338();
    const std::string lib =
        "module Lib5338; "
        "public class Item "
        "  public constructor() end constructor; "
        "end Item; "
        "public class Base "
        "  public static method Make(out owned r: Item?): bool "
        "    r := new Item(); "
        "    return true; "
        "  end Make; "
        "end Base; ";
    const std::string app =
        "module App5338; "
        "import Lib5338; "
        "public class Program "
        "  public static method Main(): int "
        "    const ok := Lib5338.Base.Make(out _); "
        "    if ok then Console.WriteLine(\"ok\"); end; "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_EQ(out, "ok\n");
}

// ── (b) Same callee, but the call's value is used in a conditional ─────────
//
// Verifies the fix works when the call's bool value is used, not just stored.
BEZEL_TEST(xmod_out_owned_discard_imported_method_5338,
           out_owned_discard_used_in_conditional_spec_22_6) {
    auto dir = wtDir5338();
    const std::string lib =
        "module Lib5338b; "
        "public class Item "
        "  public constructor() end constructor; "
        "end Item; "
        "public class Base "
        "  public static method Make(out owned r: Item?): bool "
        "    r := new Item(); "
        "    return true; "
        "  end Make; "
        "end Base; ";
    const std::string app =
        "module App5338b; "
        "import Lib5338b; "
        "public class Program "
        "  public static method Main(): int "
        "    if Lib5338b.Base.Make(out _) then "
        "      Console.WriteLine(\"called\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_EQ(out, "called\n");
}

// ── (c) Anti-regression — same-module `out _` still works ───────────────────
//
// Single-module twin to pin that the local path is unaffected.
BEZEL_TEST(xmod_out_owned_discard_imported_method_5338,
           out_owned_discard_same_module_still_works_spec_22_6) {
    const std::string prog =
        "module M5338c; "
        "class Item "
        "  public constructor() end constructor; "
        "end Item; "
        "class Base "
        "  public static method Make(out owned r: Item?): bool "
        "    r := new Item(); "
        "    return true; "
        "  end Make; "
        "end Base; "
        "class Program public static method Main(): int "
        "  const ok := Base.Make(out _); "
        "  if ok then Console.WriteLine(\"ok\"); end; "
        "  return 0; "
        "end Main; end Program;";
    auto result = bezel::test::runAot(prog);
    BEZEL_EXPECT_TRUE(result.ran);
    if (!result.ran) return;
    BEZEL_EXPECT_EQ(result.exitCode, 0);
    BEZEL_EXPECT_EQ(result.stdoutText, "ok\n");
}

// ── (d) Imported `out` (non-owned) discard still works ─────────────────────
//
// The fix also covers the plain `out _` case; verify it still works.
BEZEL_TEST(xmod_out_owned_discard_imported_method_5338,
           out_discard_non_owned_imported_method_spec_22_6) {
    auto dir = wtDir5338();
    const std::string lib =
        "module Lib5338d; "
        "public class Base "
        "  public static method TryParse(s: string, out result: int): bool "
        "    result := 42; "
        "    return true; "
        "  end TryParse; "
        "end Base; ";
    const std::string app =
        "module App5338d; "
        "import Lib5338d; "
        "public class Program "
        "  public static method Main(): int "
        "    const ok := Lib5338d.Base.TryParse(\"ignored\", out _); "
        "    if ok then Console.WriteLine(\"ok\"); end; "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_EQ(out, "ok\n");
}

}  // namespace bezel::codegen
