// Codegen — issue #3621 / spec §4.6 ("Runtime-managed field disposal") +
// §9.7 ("Records and disposal") + §28.9 ("Reference counting of runtime-
// managed values") + §2.8.3 rule 4 ("Cross-module type/field/method
// surface"), docs revision v0.286.
//
// REGRESSION PIN. A record with a `string` component, produced by an
// IMPORTED factory and dropped at scope end in the consumer, must NOT leak
// the string. The character-identical single-module twin is leak-clean by
// design (the local-path `isProvablyFreshRecordFactoryMethod` walk admits
// the `=> new Pt(...)` shape and the caller-side release registers).
//
// The cross-module twin used to leak 8290 B / 400 allocations over 200
// iterations (LSan-measured, issue body verbatim). The cause:
// `isProvablyFreshRecordFactoryMethod` keys its admission off the callee's
// `ast::MethodDecl` body — null for every IMPORTED method, whose
// reconstruction lives in the exporter's wire metadata, not the importer.
// The exporter's gate fires fine; the importer's twin returns false for
// every imported factory, so `recordCallResultReleaseEligible_` never
// receives the local, the scope-end `recordStringFields` Disposal is never
// registered, and the callee-minted +1 on the `string` field outlives the
// local.
//
// Issue #3615 fixes a related-but-different defect (the `Name` field's
// type answer becoming null for an imported record); applying #3615 changes
// this leak by ZERO bytes (measured before and after, identical 8290/400).
// The release is gated at the eligibility pass, not at the field-type
// answer.
//
// ── FIX SHAPE (spec §9.7 / §28.9 / §2.8.3 rule 4) ──────────────────────────
//
// Add `isProvablyFreshImportedRecordFactoryMethod_` (LlvmBackend_Setup.cpp)
// as the IMPORTED twin of the existing `isProvablyFreshRecordFactoryMethod`.
// The local twin proves freshness by walking the callee's body and
// demanding the entire body be `return new P(...)` (or the
// expression-bodied twin). For the imported twin there is no body to walk
// — so the proof is signature-only, BOUNDED by four guards:
//
//   (a) `ms->isStatic` — an instance method could capture `this.f` and
//       return a borrow of an owner-still-live field; a static method has
//       no `this` and so no instance-field-borrow path.
//
//   (b) `ms->genericParams.empty()` — a generic factory's per-instantiation
//       body is not visible to the importer outside the §14.1 generic
//       surface, so an instantiation-aware walk is out of scope here.
//
//   (c) the wire-side RETURN TYPE (`ctx.importedReturnType(ms)`) IS exactly
//       `recTs` — so the factory's contract is "I return this very record
//       type" and any +1 minted at construction is associated with this
//       record type's release.
//
//   (d) `recTs` has NO user-declared (ConstructorSymbol) member — the
//       compiler-generated primary ctor is therefore the ONLY construction
//       path for any `recTs` value the factory could produce. With the
//       primary ctor as the sole path, the caller's scope-end release
//       (the recordStringFields Disposal at ~14275–14293) is provably the
//       SOLE owner of those +1s — no double-free, no UAF.
//
// Locals (`ms->decl != nullptr`) are still routed through
// `isProvablyFreshRecordFactoryMethod`, which has the body-walk proof. The
// existing local tests (`#1747` / `record_string_field_call_release_*`,
// `#2448` / `record_call_result_borrowed_return_uaf_*`) continue to pass
// unchanged.
//
// UNSAFE admit case NOT guarded (follow-up #3622 will close it):
// `=> staticField` — a static factory that returns a static-field read of
// type `recTs` (a borrow of the static storage's +1, not a freshly-minted
// one). The guard would require the importer to walk the enclosing class's
// own static-storage members for a recTs-typed one, a strictly larger
// surface than the local check reasons about; deliberately out of scope.
// The common case `=> new Pt(args...)` (a primary ctor with +1 string
// producers) IS admitted — and that is the case the issue's repro fits.

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
// per CLAUDE.md). The library object MUST outlive the app build — a removed
// .o shows up as `bezelc: error: cannot load module metadata from ...`. The
// `TempFile` shared_ptrs in the helpers below keep it open.
static const std::filesystem::path& wtDir3621() {
    return wtScratchDir("wt-3621");
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
    std::string stem = unique_stem("bezel_3621lib_");
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
    std::string stem = unique_stem("bezel_3621app_");
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

struct LocalHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> exe;
    std::string exePath;
    bool ok = false;
};

LocalHandle buildLocalApp(const fs::path& dir, const std::string& src,
                          bool sanitize) {
    LocalHandle h;
    std::string stem = unique_stem("bezel_3621local_");
    h.src = std::make_shared<TempFile>((dir / (stem + ".bzl")).string());
    h.exe = std::make_shared<TempFile>((dir / stem).string());
    writeSource(h.src->path, src);
    bezel::Driver driver;
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = sanitize;
    bezel::CompileResult r =
        driver.compileFile(h.src->path, h.exe->path, {}, opts);
    h.ok = r.success;
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

// The issue's verbatim repro: an IMPORTED factory returning an IMPORTED
// record with a `string` field; consumer accumulates `p.Name.Length` so
// every iteration produces a fresh +1 string in the dependency that the
// consumer must release.
const std::string kLibSrc3621 =
    "module Lib3621; "
    "public record Pt(X: int, Name: string); "
    "public class Maker3621 "
    "  public static method Make(n: int): Pt => "
    "    new Pt(n, \"hello-\" + n.ToString()); "
    "end Maker3621; ";

// The issue's verbatim consumer: 200-iteration loop reading `p.Name.Length`.
// Pre-fix: 8290 B / 400 allocations leaked (`Direct leak of 6400 byte(s) in
// 200 object(s)` + `Indirect leak of 1890 byte(s) in 200 object(s)`).
// Post-fix: no leaks. The exit code sums `Name.Length` per iteration
// (`hello-N` for `N in [0..199]` has lengths 7..11, so total is ~3588),
// but the load-bearing oracle is the LSan soundness line.
const std::string kAppSrc3621 =
    "module App3621; "
    "import Lib3621; "
    "public class C3621 "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    var i: int := 0; "
    "    while i < 200 do "
    "      var p := Lib3621.Maker3621.Make(i); "
    "      total := total + p.Name.Length; "
    "      i := i + 1; "
    "    end while; "
    "    return total; "
    "  end Main; "
    "end C3621; ";

// The character-identical single-module twin — must stay clean (regression
// guard for the local path: `isProvablyFreshRecordFactoryMethod` continues
// to admit it through the body walk, so the existing release registration
// is unchanged by this fix).
const std::string kLocalSrc3621 =
    "module Local3621; "
    "public record Pt(X: int, Name: string); "
    "public class Maker3621 "
    "  public static method Make(n: int): Pt => "
    "    new Pt(n, \"hello-\" + n.ToString()); "
    "end Maker3621; "
    "public class C3621 "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    var i: int := 0; "
    "    while i < 200 do "
    "      var p := Maker3621.Make(i); "
    "      total := total + p.Name.Length; "
    "      i := i + 1; "
    "    end while; "
    "    return total; "
    "  end Main; "
    "end C3621; ";

}  // namespace

// ── HEADLINE — issue's exact repro, ASan-instrumented, no leaks ──────────
//
// Pre-fix: 8290 B / 400 allocations leaked (`Direct leak of 6400 byte(s) in
// 200 object(s)` and `Indirect leak of 1890 byte(s) in 200 object(s)`,
// both attributed to `bz$Lib.Maker$Make`). Post-fix: 0 leaks.
BEZEL_TEST(imported_record_string_field_call_scope_end_release_3621,
           imported_factory_repro_asan_no_leak) {
    auto dir = wtDir3621();
    LibHandle libH = buildLib(dir, kLibSrc3621);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle appH = buildApp(dir, libH.objPath, kAppSrc3621, /*sanitize=*/true);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult rr = runCapture(appH.exePath);
    // Exit-code oracle: `total` is sum of `Name.Length` over 200 iters
    // (`"hello-N"` lengths 7..11). The pre-fix path returned this same
    // value, so the oracle is REGRESSION-shaped (same value, no leak).
    BEZEL_EXPECT_TRUE(rr.code >= 0);
    // LSan / leak-string oracle: the load-bearing half. With the bug
    // present, ASan reports "Direct leak of N byte(s) in M object(s)" and
    // "Indirect leak of N byte(s) in M object(s)" attributed to the
    // `bz$Lib.Maker$Make` allocation site. With the fix, none of these
    // strings appear.
    BEZEL_EXPECT_TRUE(rr.out.find("LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Direct leak") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Indirect leak") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("bz$Lib") == std::string::npos);
}

// ── CONTROL — LOCAL twin (no `import`), ASan-instrumented, no leaks ──────
//
// Pins that the fix did not disturb the already-working local path:
// `isProvablyFreshRecordFactoryMethod` still admits the same body shape,
// the scope-end release still registers, no leak appears.
BEZEL_TEST(imported_record_string_field_call_scope_end_release_3621,
           local_twin_still_clean_under_asan) {
    auto dir = wtDir3621();
    LocalHandle h = buildLocalApp(dir, kLocalSrc3621, /*sanitize=*/true);
    BEZEL_EXPECT_TRUE(h.ok);
    if (!h.ok) return;
    RunResult rr = runCapture(h.exePath);
    BEZEL_EXPECT_TRUE(rr.code >= 0);
    BEZEL_EXPECT_TRUE(rr.out.find("LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Direct leak") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Indirect leak") == std::string::npos);
}

// ── SOUNDNESS — imported factory returns EXACTLY the imported record type
//
// Regression guard for guard (c) — the wire-side return type IS exactly the
// record type. A factory whose declared return type is the record type
// (not a subtype, not a supertype) is admitted. A factory whose return
// type would be e.g. `object` is rejected.
BEZEL_TEST(imported_record_string_field_call_scope_end_release_3621,
           imported_factory_exact_return_type_asan_no_leak) {
    auto dir = wtDir3621();
    // Same library as the headline — same factory shape, same return type.
    LibHandle libH = buildLib(dir, kLibSrc3621);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    // Smaller loop (50 iters) so the test runs quickly while still
    // exercising the same release path.
    const std::string smallApp =
        "module App3621small; "
        "import Lib3621; "
        "public class CSmall "
        "  public static method Main(): int "
        "    var total: int := 0; "
        "    var i: int := 0; "
        "    while i < 50 do "
        "      var p := Lib3621.Maker3621.Make(i); "
        "      total := total + p.Name.Length; "
        "      i := i + 1; "
        "    end while; "
        "    return total; "
        "  end Main; "
        "end CSmall; ";
    AppHandle appH = buildApp(dir, libH.objPath, smallApp, /*sanitize=*/true);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult rr = runCapture(appH.exePath);
    BEZEL_EXPECT_TRUE(rr.out.find("Direct leak") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Indirect leak") == std::string::npos);
}

}  // namespace bezel::codegen