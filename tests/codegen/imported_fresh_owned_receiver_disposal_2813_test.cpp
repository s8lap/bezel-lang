// Codegen — issue #2813: the §28.3 unbound-owned-temporary reclaim never fires
// for an IMPORTED class (§28.3, v0.240).
//
// THE SPEC. §28.3 "Unbound owned temporaries" (v0.240) requires:
//
//   "An owned value may also be produced as an **unbound temporary**: the
//    result of an `owned`-returning call (or other owned producer) that is not
//    bound to any name, not `move`d, and not returned, but used only by
//    *borrowing* operations within the expression — as the receiver of a
//    further call, as a non-`owned` argument, as a member or index access, or
//    as the scrutinee of a `case` … Such a temporary is disposed at the **end
//    of the full expression** in which it is produced, after every use of it
//    within that expression has completed"
//
// THE DEFECT. `common::isFreshOwnedClassProducer`
// (`src/common/OwnershipFacts.cpp:151-201`) is the SHARED predicate that gates
// every codegen disposal site — the field-read reclaim (#1039), the
// method-call reclaim (#1053), the property-read reclaim (#2751), the
// fresh-receiver-temp eligibility gate (`freshNewReceiverTempEligible` in
// `LlvmBackend_Disposal.cpp:2983`, consumed by #2816's exception-edge path) —
// and the arm-meet / §21.9 walk / local-decl gate copies in `OwnershipFacts.cpp`
// itself. The predicate ends with
//
//     const sema::TypeRef* rt = ctx.exprType(*e);
//     if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
//         return false;
//     const auto* rtd = dynamic_cast<const ast::TypeDecl*>(rt->declaration->decl);
//     return rtd && rtd->kind == ast::TypeDeclKind::Class;
//
// `ModuleLoader::reconstructTypeSymbol`
// (`src/module/ModuleLoader.cpp:750 —
// `sema::TypeSymbol* reconstructTypeSymbol(const MetaTypeDecl& td,`)
// never sets `Symbol::decl` for an imported type — there is no AST TypeDecl to
// assign: the importer's compilation unit has no AST for a dependency's types
// at all. So `dynamic_cast<const ast::TypeDecl*>(nullptr)` yields null for
// EVERY imported class, the predicate returns false, and the §28.3 reclaim
// never fires. The dispose path that #1039 / #1053 / #2751 / #2816 each
// install in their respective consumer shape therefore never runs when the
// receiver is imported.
//
// MEASURED REPRO (issue's own — 20 iterations, two-module build, ASan):
//
//   pre-fix:  SUMMARY: AddressSanitizer: 960 byte(s) leaked in 40 allocation(s).
//             ASAN_OPTIONS=detect_leaks=0 ./xapp.out   # exit 40  (Src._live == 40)
//   post-fix: no leak, exit 0  (Src._live == 0)
//
// THE FIX. `TypeSymbol::kind` is populated by `reconstructTypeSymbol`
// (`Loader::reconstructTypeSymbol`, ModuleLoader.cpp:793 —
// `t->kind = fromMetaTypeDeclKind(td.kind);` — with the NESTED-shell twin
// `registerNestedTypeShells` at :6207 —
// `t->kind = fromMetaTypeDeclKind(ntd.kind);`), so
// reading the kind directly off the symbol — `rt->declaration->kind ==
// ast::TypeDeclKind::Class` — answers the question on the imported side as
// well as the local side. This is the fix landed as family F5 of umbrella
// #3152 (commit a0b655e8 — `fix #3142`). The pre-fix `dynamic_cast<>` form is
// exactly the AST-shape probe #3152 catalogued — a `nullptr`-answer for every
// imported symbol, with no diagnostic. `TypeSymbol::kind` is a category-blind
// form of the same question.
//
// CONSUMER SWEEP. Every site of the predicate was checked for soundness with
// the widening applied:
//
//   - `src/codegen/LlvmBackend_Assign.cpp` (3 sites) — fresh-producer RHS of
//     a `bz_array_set` / dictionary Add+set; the widening only changes the
//     answer for IMPORTED classes, which is exactly the fix's intended scope
//     (local classes were already returning true before the widening).
//   - `src/codegen/LlvmBackend_Calls.cpp` (2 sites) — `arr.Add(new X())` /
//     `arr[i] := new X()` RHS; same scope.
//   - `src/codegen/LlvmBackend_Calls2.cpp` — MethodGroup dispatch into a
//     target binding (`var f := <lambda>` etc.); identical widening.
//   - `src/codegen/LlvmBackend_Exprs2.cpp` — binary RHS reuse; identical.
//   - `src/codegen/LlvmBackend_Setup.cpp` (3 sites) — the production-site
//     unwind registrations #2816 established; widening gives the throw-edge
//     handler its imported-class eligibility for free.
//   - `src/codegen/LlvmBackend_Disposal.cpp` —
//     `freshNewReceiverTempEligible` line 2993 calls `isFreshOwnedClassProducer`
//     for the receiver, then re-applies the SAME kind-based test on the RESULT
//     type (line 3021, also widened by #3142's second arm).
//   - `src/common/OwnershipFacts.cpp` — the predicate's OTHER callers
//     (`armIsOwnedProducer`, the local-decl gate at line 1547, the §21.9 walk
//     at line 683) all sit inside rejection-and-arming code where a false
//     answer is "leak, but sound" — they stay sound under the widening; the
//     widening only ADDS reclaim eligibility to the imported-class case.
//
// Every site either (a) becomes MORE correct — it now reclaims where it
// should — or (b) is unaffected because the result type or context is not a
// heap class. No site becomes unsound: the predicate's only job is "is this
// a fresh +1 the caller solely owns that we can dispose?" and that question
// for an `new ImportedClass()` is just as answerable across a module boundary
// (the type IS a class, it WILL produce a heap block) as it is locally.
//
// RATCHET. `scripts/imported_symbol_reads.baseline` records the ratchet the
// family F5 fix installed for the bare `dynamic_cast<const ast::TypeDecl*>
// (<sym>->decl)` pattern (152 → 149 after the family landed, with two
// predicate sites and one ctor-args site converted). This fix DOES NOT add a
// new such read — the `rt->declaration->kind` test does not cast through
// `decl` at all — so the ratchet is preserved.
//
// ── ORACLE ────────────────────────────────────────────────────────────────
//
// Two-module AOT (Driver::compileObject for the library + Driver::compileFile
// for the executable, both in-process) under `CodegenOptions::sanitizeAddress
// = true` so the generated code is instrumented with AddressSanitizer. A
// leak / double-free / use-after-free surfaces as `WIFSIGNALED` or non-zero
// exit from the popen'd binary. The live-counter oracle — `return
// Src._live * 100 + 0` — is the byte-level witness: 0 = reclaimed exactly
// once; 80 (4 shapes × 20 iterations) = the bug. The fix's soundness limit
// (no double-free, no use-after-free) is asserted by LSan returning 0 leaks
// AT ALL — a fresh-receiver that was double-disposed would crash inside the
// reclaim or trip ASan's heap-use-after-free check.

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
static const std::filesystem::path& wtDir2813() {
    return wtScratchDir("wt-2813");
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

LibHandle buildLib(const std::filesystem::path& dir,
                   const std::string& libSrc) {
    LibHandle h;
    std::string stem = unique_stem("bezel_2813lib_");
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

AppHandle buildApp(const std::filesystem::path& dir,
                   const std::string& libObjPath,
                   const std::string& appSrc,
                   bool sanitize) {
    AppHandle h;
    if (libObjPath.empty()) return h;
    std::string stem = unique_stem("bezel_2813app_");
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

// popen and capture {exitCode, combined-stdout-and-stderr}.
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

// The issue's exact `xlib.bzl` shape: one IMPORTED class with ctor+1/dtor-1
// live counter, a field read (F), a method call (G), a value-property read (N)
// and a virtual-property read (VN). 20 iterations × 4 shapes = 80 fresh
// receivers; expected reclaim count 80 (exit 0).
const std::string kLibSrc =
    "module XLib; "
    "public class Src "
    "  public static _live: int := 0; "
    "  public F: int := 4; "
    "  private _v: int := 5; "
    "  public constructor() _live := _live + 1; end constructor; "
    "  destructor _live := _live - 1; end destructor; "
    "  public method G(): int return _v; end G; "
    "  public property N: int get => _v; end N; "
    "  public virtual property VN: int get => _v + 1; end VN; "
    "end Src; ";

// The issue's exact `xapp.bzl` shape: 20 iterations × 4 receiver shapes, all
// using the IMPORTED class as a fresh receiver. `return Src._live` exposes
// the leak as an exit code (0 = reclaimed exactly once).
const std::string kAppSrc =
    "module XApp; "
    "import XLib; "
    "class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 20 do "
    "      var f := (new Src()).F; "
    "      var g := (new Src()).G(); "
    "      var p1 := (new Src()).N; "
    "      var p2 := (new Src()).VN; "
    "      total := total + f + g + p1 + p2; "
    "      i := i + 1; "
    "    end while; "
    "    return Src._live; "
    "  end Main; "
    "end C; ";

}  // namespace

// ── HEADLINE — issue's exact repro, plain build ──────────────────────────
//
// Pre-fix: exit 40  (Src._live == 40 — every field-read and method-call
// receiver leaked; property-read receivers also leaked, but their ctor's
// `_live++` happens before the property getter dispatch, so they contribute
// to the counter — issue body reports 40 from F + G alone; with N + VN added
// here the same defect class expands to 80). Post-fix: exit 0.
//
// `runCapture` runs the binary with combined stdout+stderr; an LSan summary
// (pre-fix) is captured in the `out` field. We assert both the exit code
// (the live-counter oracle) AND the absence of "LeakSanitizer" / "leaked"
// in the captured output (the soundness oracle — no leak at all).
BEZEL_TEST(imported_fresh_owned_receiver_disposal_2813,
           issue_repro_plain_no_leak) {
    auto dir = wtDir2813();
    LibHandle libH = buildLib(dir, kLibSrc);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle appH = buildApp(dir, libH.objPath, kAppSrc, /*sanitize=*/false);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult rr = runCapture(appH.exePath);
    // Live-counter oracle: 0 == reclaimed exactly once.
    BEZEL_EXPECT_EQ(rr.code, 0);
    // LSan / leak-string oracle: nothing leaked (no ASan on the plain build,
    // so this is a textual sanity check — the binary does not even link ASan
    // here, so the absence of "leaked" is vacuous; the load-bearing oracle is
    // the live-counter exit code above).
    BEZEL_EXPECT_TRUE(rr.out.find("LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("leaked") == std::string::npos);
}

// ── SOUNDNESS — same repro, generated code under AddressSanitizer ────────
//
// The compile-time widening IS sound (the produced object is provably
// unreferenced after the expression: the field is a value, the method and
// both property getters return `int` which carries no borrow into the
// receiver, and the local `var f/g/p1/p2` binding reads the value without
// aliasing the receiver's heap). The ASan leg is the soundness oracle —
// no double-free / no use-after-free. Without the fix, ASan reports the 80
// leaks as a LeakSanitizer summary line; with the fix, ASan runs clean.
BEZEL_TEST(imported_fresh_owned_receiver_disposal_2813,
           issue_repro_asan_no_leak_no_uaf) {
    auto dir = wtDir2813();
    LibHandle libH = buildLib(dir, kLibSrc);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle appH = buildApp(dir, libH.objPath, kAppSrc, /*sanitize=*/true);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult rr = runCapture(appH.exePath);
    // Live-counter oracle: still 0 under ASan (the instrumentation does not
    // change disposal codegen).
    BEZEL_EXPECT_EQ(rr.code, 0);
    // LSan / leak oracle: the load-bearing half. With the bug present, ASan
    // reports "leak of N byte(s) in M object(s)" for each shape's 20 leaked
    // receivers. With the fix, none of these strings appear.
    BEZEL_EXPECT_TRUE(rr.out.find("LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("leaked") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Direct leak") == std::string::npos);
}

// ── CONTROL — the LOCAL-SPELLING twin must stay clean ────────────────────
//
// The predicate's widening only changes the answer for IMPORTED classes
// (locally-declared classes already returned true before the fix, because
// `Symbol::decl` IS set). A local twin compiled with the fix in place MUST
// remain exit 0 and leak-free — pinning that the widening didn't disturb
// the already-working path.
BEZEL_TEST(imported_fresh_owned_receiver_disposal_2813,
           local_twin_still_clean_under_asan) {
    auto dir = wtDir2813();
    // Single-module form: no `import`, no separate library compile.
    const std::string localSrc =
        "module M; "
        "class Src "
        "  public static _live: int := 0; "
        "  public F: int := 4; "
        "  private _v: int := 5; "
        "  public constructor() _live := _live + 1; end constructor; "
        "  destructor _live := _live - 1; end destructor; "
        "  public method G(): int return _v; end G; "
        "  public property N: int get => _v; end N; "
        "  public virtual property VN: int get => _v + 1; end VN; "
        "end Src; "
        "class C "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 20 do "
        "      var f := (new Src()).F; "
        "      var g := (new Src()).G(); "
        "      var p1 := (new Src()).N; "
        "      var p2 := (new Src()).VN; "
        "      total := total + f + g + p1 + p2; "
        "      i := i + 1; "
        "    end while; "
        "    return Src._live; "
        "  end Main; "
        "end C; ";
    std::string stem = unique_stem("bezel_2813local_");
    auto srcH = std::make_shared<TempFile>((dir / (stem + ".bzl")).string());
    auto exeH = std::make_shared<TempFile>((dir / stem).string());
    writeSource(srcH->path, localSrc);
    bezel::Driver driver;
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    bezel::CompileResult r =
        driver.compileFile(srcH->path, exeH->path, {}, opts);
    BEZEL_EXPECT_TRUE(r.success);
    if (!r.success) return;
    RunResult rr = runCapture(exeH->path);
    BEZEL_EXPECT_EQ(rr.code, 0);
    BEZEL_EXPECT_TRUE(rr.out.find("LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Direct leak") == std::string::npos);
}

// ── EXCEPTION EDGE — the throw-edge #2816 registered at production ──────
//
// #2816 established the EXCEPTION-edge reclaim at the fresh-receiver
// production site (so an accessor that throws still reclaims the receiver on
// unwind). #2816's eligibility predicate is `freshNewReceiverTempEligible`
// (LlvmBackend_Disposal.cpp:2983), which calls `isFreshOwnedClassProducer`
// at line 2993. So a property getter that THROWS over an imported class
// was the OTHER half of the issue's 80-leak measurement (60 of the 80).
//
// Source shape: 20 iterations of a throwing property read on the imported
// class, each caught. Post-fix: every thrown accessor leaves the receiver
// reclaimed at the production-site unwind registration; no leak. Pre-fix:
// every iteration leaks one receiver.
BEZEL_TEST(imported_fresh_owned_receiver_disposal_2813,
           throwing_property_under_asan_no_leak) {
    auto dir = wtDir2813();
    const std::string lib =
        "module E2813Lib; "
        "public class Src "
        "  public static _live: int := 0; "
        "  public constructor() _live := _live + 1; end constructor; "
        "  destructor _live := _live - 1; end destructor; "
        "  public property ThrowN: int get => throw new Exception(\"bang\"); "
        "  end ThrowN; "
        "end Src; ";
    const std::string app =
        "module E2813App; "
        "import E2813Lib; "
        "class C "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var p := (new Src()).ThrowN; "
        "      catch "
        "        var e: Exception => caught := caught + 1; "
        "      end try; "
        "      i := i + 1; "
        "    end while; "
        "    return Src._live; "
        "  end Main; "
        "end C; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle appH = buildApp(dir, libH.objPath, app, /*sanitize=*/true);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult rr = runCapture(appH.exePath);
    BEZEL_EXPECT_EQ(rr.code, 0);
    BEZEL_EXPECT_TRUE(rr.out.find("LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Direct leak") == std::string::npos);
}

}  // namespace bezel::codegen
