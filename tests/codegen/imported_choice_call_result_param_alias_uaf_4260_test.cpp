// Codegen — issue #4260 / spec §9.7 ("Records and disposal") + §28.9
// ("Runtime-managed types") + §2.8.3 rule 4 ("Cross-module type/field/method
// surface"), docs revision v0.298.
//
// REGRESSION PIN — the IMPORTED axis of fix #4254.
//
// `fix #4254` (record_choice_call_result_param_alias_uaf_4254_test.cpp)
// narrowed `Emitter::walkCallArgs`'s #1430 relaxation so a plain `string`
// argument is NOT treated as a call-bounded borrow when the resolved
// callee's declared return type is a record or a value-like choice — such a
// callee can embed the argument into a freshly-constructed value with a bare
// `CreateStore` (no `emitStringRetain`), aliasing the caller's string with no
// independent +1. That fix read the return type via a LOCAL-ONLY cast,
// `dynamic_cast<const ast::MethodDecl*>(ms->decl)`, and left the IMPORTED
// axis (`ms->decl == nullptr`) unhandled — filed as this issue.
//
// THE GAP WAS NARROWER THAN THE ISSUE'S OWN (explicitly unmeasured) repro
// assumed. `fix #3645` already widens `ModuleMetadataBuilder`'s wire export
// to ship `methodBodyText` — and so populate the imported `MethodSymbol`'s
// `decl` via `ModuleLoader`'s re-parse — for any NON-GENERIC method whose
// declared return type is a RECORD (`isNonGenericClassRecordReturn`,
// ModuleMetadataBuilder.cpp ~1296). So a record-returning imported factory
// shaped like #4260's own repro (`Helper.Wrap(s: string): Str`) ALREADY has
// a non-null `ms->decl` by the time `walkCallArgs` runs, and the pre-fix
// LOCAL-only cast ALREADY found the record return type — MEASURED (this
// fix's own worktree, `BEZEL_DEBUG_4260`-instrumented build): for that exact
// shape, `calleeReturnMayAliasArgUnretained` was TRUE on both sides of this
// fix, and the cross-module run showed a leak (not a UAF) identically
// before and after — a PRE-EXISTING, UNRELATED gap (see issue 4266), not
// this one.
//
// `fix #3645`'s widening checks `ast::TypeDeclKind::Record` specifically —
// NOT Choice — so a non-generic method on a non-generic class returning a
// value-like CHOICE is NOT covered: `methodBodyText` stays unshipped for
// that shape, `ms->decl` stays genuinely null, and the pre-fix LOCAL-only
// cast never fires. MEASURED (same instrumented build, this file's repro
// below): `ms->decl == nullptr`, `calleeReturnMayAliasArgUnretained == 0`
// (the record/choice gate never fired), `safeArg == 1` (the #1430 relaxation
// WAS admitted) — so the caller's `s` was marked call-bounded and released
// at its own (narrower) scope end while the choice case value returned by
// `Wrap` aliased it with no independent +1.
//
// MEASURED FAIL-WITHOUT (this fix's own worktree, isolated: `git diff`
// captured, `git checkout --` to strip the fix, `cmake --build build
// --target bezelc` [not just the test target — the repro below shells out
// to a two-step `build/bezelc -c` / `build/bezelc --import` compile, the
// standalone executable, per LANE_RULES §23], re-run under
// `scripts/dev_heavy.sh probe ... --sanitize=address`, `git apply` to
// restore): the cross-module CHOICE repro is a real `AddressSanitizer:
// heap-use-after-free` — READ of size 8, freed in `main`, previously
// allocated in `main`, a 32-byte `bz.string`-header region — the SAME
// signature `record_choice_call_result_param_alias_uaf_4254_test.cpp`'s
// three local-axis rows measure. PASS-WITH (this fix, same rebuild-and-
// rerun cycle): clean, exit 5 (`"hello".Length`), no ASan/LSan output at
// all. 1 of 1 row fails without the fix and passes with it.
//
// FIX: `walkCallArgs` now branches on `ctx.isImported(ms)` FIRST — mirroring
// the established local/imported split `isUserMethodStringReturnCall` /
// `isUserMethodClosureReturnCall` (this same file, a few hundred lines away)
// already use — and reads the declared return type via
// `ctx.importedReturnType(ms)` for the imported leg (the wire-side channel:
// `TypeSymbol::kind` is published identically for an imported Record/Choice,
// so `crt->declaration->kind` reads the same on both legs). The local-only
// cast is now reached only on the local leg and carries the same
// `SEAM-ACCESSOR` marker the two sibling predicates use — this read is the
// sanctioned local-path accessor now that the imported leg has its own.
//
// `check_imported_symbol_reads.py`'s baseline is back to 10 for this file
// (the `SEAM-ACCESSOR` marker exempts the local-path cast from the class-3
// count) — no `--force` needed; `--strict` passes clean on the reverted
// baseline.
//
// See tests/codegen/imported_record_string_field_call_scope_end_release_3621_test.cpp
// for the two-module build harness this file's LibHandle/AppHandle helpers
// mirror, and record_choice_call_result_param_alias_uaf_4254_test.cpp for
// the local-axis siblings (PIN 1/2/3, including the identical Choice shape
// PIN 3 pins for the same-module case).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::codegen {
namespace {

namespace fs = std::filesystem;

// Per-process unique working dir under the shared scratch base (DISK-backed
// per CLAUDE.md), mirroring wtDir3621() in the #3621 test file.
static const std::filesystem::path& wtDir4260() {
    return wtScratchDir("wt-4260");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream(path) << src;
}

struct LibHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle buildLib(const fs::path& dir, const std::string& libSrc) {
    LibHandle h;
    std::string stem = unique_stem("bezel_4260lib_");
    h.src = std::make_shared<TempFile>((dir / (stem + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile>((dir / (stem + "_lib.o")).string());
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
    std::string exePath;
    bool ok = false;
};

AppHandle buildApp(const fs::path& dir, const std::string& libObjPath,
                   const std::string& appSrc, bool sanitize) {
    AppHandle h;
    if (libObjPath.empty()) return h;
    std::string stem = unique_stem("bezel_4260app_");
    h.src = std::make_shared<TempFile>((dir / (stem + "_app.bzl")).string());
    h.exe = std::make_shared<TempFile>((dir / stem).string());
    writeSource(h.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjPath;
    d.objectPath = libObjPath;
    deps.push_back(d);
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = sanitize;
    bezel::CompileResult rb = driver.compileFile(h.src->path, h.exe->path,
                                                  deps, opts);
    h.ok = rb.success;
    h.exePath = h.exe->path;
    return h;
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

// The narrow-scope shape #4260's own issue text used verbatim, transplanted
// to a value-like CHOICE return (the channel #3645's wire-export widening
// does NOT cover — see file header). `Helper` is a non-generic class on a
// non-generic method, so `ms` survives walkCallArgs' top-of-function guard
// (`ms->decl == nullptr && ctx.isImported(ms)` — not excluded) and reaches
// the record/choice gate with a genuinely null `ms->decl`.
const std::string kLibSrc4260 =
    "module LibChoice4260; "
    "public choice Tag4260 Named(V: string); end Tag4260; "
    "public class HelperChoice4260 "
    "  public static method Wrap(s: string): Tag4260 "
    "    return Tag4260.Named(s); "
    "  end Wrap; "
    "end HelperChoice4260; ";

// `outer` is declared OUTSIDE the `do ... end;` block that owns `s`, then
// ASSIGNED inside it (not `var outer := ...` — the narrower-scope shape is
// only reachable this way, since a `var` bound inside the block would go out
// of scope with `s`). Matches PIN 1/PIN 3's shape in the #4254 local-axis
// test file, transplanted across the module boundary.
const std::string kAppSrc4260 =
    "module AppChoice4260; "
    "import LibChoice4260; "
    "public class CChoice4260 "
    "  public static method Main(): int "
    "    var outer: Tag4260; "
    "    do "
    "      var s := \"he\" + \"llo\"; "
    "      outer := LibChoice4260.HelperChoice4260.Wrap(s); "
    "    end; "
    "    return (case outer of Named(v) => v.Length); "
    "  end Main; "
    "end CChoice4260; ";

}  // namespace

// ── HEADLINE — cross-module CHOICE axis, ASan-instrumented, no UAF ───────
//
// FAIL-WITHOUT (measured, this fix's own worktree, see file header): a real
// `AddressSanitizer: heap-use-after-free`, READ of size 8, freed in `main`,
// previously allocated in `main` — the identical 32-byte `bz.string`-header
// signature record_choice_call_result_param_alias_uaf_4254_test.cpp's local
// rows measure. PASS-WITH: clean, exit 5, no ASan/LSan output. The
// load-bearing oracle is the ABSENCE of the UAF signature — this shape has
// no pre-existing, unrelated leak (unlike the record-return channel, which
// #3645 already routes onto the local-cast path and which leaks 38 B/2
// allocs identically before and after this fix via an unrelated gap, filed
// as issue 4266).
BEZEL_TEST(imported_choice_call_result_param_alias_uaf_4260,
           narrow_scope_imported_choice_wrap_no_longer_uaf) {
    auto dir = wtDir4260();
    LibHandle libH = buildLib(dir, kLibSrc4260);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle appH = buildApp(dir, libH.objPath, kAppSrc4260, /*sanitize=*/true);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult rr = runCapture(appH.exePath);
    // Correctness oracle: "he"+"llo" = "hello", Length 5 — the SAME value
    // the pre-fix (UAF-corrupted) run could not reliably reach at all.
    BEZEL_EXPECT_EQ(rr.code, 5);
    // Soundness oracle — the load-bearing half. Pre-fix this string is
    // present (ASan aborts before printing an exit code `popen`/`pclose`
    // could distinguish from a clean run by code alone).
    BEZEL_EXPECT_TRUE(rr.out.find("heap-use-after-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("AddressSanitizer") == std::string::npos);
}

}  // namespace bezel::codegen
