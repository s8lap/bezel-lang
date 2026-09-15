// Codegen — issue #4279 / spec §9.7 ("Records and disposal") + §28.9
// ("Runtime-managed types") + §2.8.3 rule 4 ("Cross-module type/field/method
// surface"), docs revision v0.298.
//
// REGRESSION PIN — the IMPORTED axis of fix #4253.
//
// `fix #4253` (class_field_record_string_release_4253_test.cpp) added the
// class-field-record-string release walk in `Emitter::emitOwnedFieldDisposals`
// (LlvmBackend_Disposal.cpp), gated on the field's OWN declaration initializer
// being a provably fresh construction (`isFreshRecordConstructionOfExactType`).
// That commit read the initializer via a LOCAL-ONLY pair of casts,
// `dynamic_cast<const ast::FieldDecl/PropertyDecl*>(m->decl)`, which is always
// null for an IMPORTED member (no `ast::FieldDecl`/`PropertyDecl` crosses the
// module boundary — the #3671 seam) — so the new release NEVER fired for an
// imported field, silently: `check_imported_symbol_reads.py`'s class-3 ratchet
// caught the two new reads and the fix banked them as `SEAM-DEBT: #4279`
// (a deliberate, marked, safe-direction gap: the field kept its PRE-EXISTING
// leak, never a wrong release).
//
// THE GAP IS REACHABLE ONLY THROUGH A GENERIC declaring class. §7.9's own
// synthesized `bz$<Type>$init` already replays a NON-generic class's own
// instance field initializers, so `emitOwnedFieldDisposals` is only ever
// re-invoked with `ts->members` holding a decl-less (`m->decl == nullptr`)
// member when `ts` itself is an IMPORTED GENERIC class being monomorphized in
// THIS (importing) compilation — the same shape #2874's own
// `importedInstanceFieldInitExpr` side table was built for
// (`ModuleMetadataBuilder.cpp`'s `fieldInitText` writer gate is
// `isGenericType || (isRecordType && !isStaticField)`; a non-generic,
// non-record declaring class carries no field-init text across the boundary
// at all, so there is nothing this fix's fallback COULD recover for it — see
// the send-back's own "does the fallback resolve #4279 at all" question,
// answered YES for exactly this generic-declaring-class shape).
//
// FIX (this file's own commit, a send-back on `fix #4253`'s review): add the
// SAME `m->decl == nullptr` fallback `emitOwnFieldInit` (LlvmBackend_Setup.cpp)
// and the record-preamble init walk (LlvmBackend_Exprs2.cpp) already use —
// `ctx_.importedInstanceFieldInitExpr(m)`, the #2874 load-time-reconstructed
// `ast::Expr*` for exactly this member — reused verbatim, not re-derived (per
// LANE_RULES §6/§25). Both `dynamic_cast` reads are marked `SEAM-ACCESSOR`
// (not `SEAM-DEBT`): this fallback IS the sanctioned both-sources accessor for
// this arm, so `check_imported_symbol_reads.py`'s baseline is back to 3 for
// `LlvmBackend_Disposal.cpp` (was 5 under the prior `--force`d rise) — no
// `--force` needed, `--strict` passes clean on the reverted baseline.
//
// MEASURED FAIL-WITHOUT (this send-back's own worktree, isolated:
// `git checkout HEAD -- src/codegen/LlvmBackend_Disposal.cpp` to restore fix
// #4253's ORIGINAL committed (SEAM-DEBT, no-fallback) version -- this file's
// own fix was still uncommitted staged-working-tree at that point, so HEAD
// was `12e6f1e` itself -- rebuilt `bezel_tests_codegen`, re-ran the headline
// row below with a temporary debug print of the captured process output,
// then restored this fix from a scratch-copy backup and rebuilt again):
//     Direct leak of 6368 byte(s) in 199 object(s)
//     Indirect leak of 796 byte(s) in 199 object(s)
//     SUMMARY: AddressSanitizer: 7164 byte(s) leaked in 398 allocation(s).
// PASS-WITH (this fix, same rebuild-and-rerun cycle): clean, exit 88
// (600 % 256), no ASan/LSan output at all. 1 of 1 row fails without the fix
// and passes with it. The assertion below is a STRING-ABSENCE oracle (no
// `Direct leak` / `LeakSanitizer` / `AddressSanitizer` substring), not a
// byte-count assertion, so LANE_RULES §30's conservative-stack-scan flake
// concern (which is about NUMERIC floors) does not apply here; the 199-vs-200
// count above (not 200/200) is itself scan noise of exactly that kind and is
// not load-bearing for either oracle.
//
// See tests/codegen/imported_record_string_field_call_scope_end_release_3621_test.cpp
// for the two-module build harness this file's LibHandle/AppHandle helpers
// mirror, and tests/codegen/imported_choice_call_result_param_alias_uaf_4260_test.cpp
// for the established "REGRESSION PIN — the IMPORTED axis of fix #N" framing
// this file follows.

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
// per CLAUDE.md), mirroring wtDir3621()/wtDir4260() in the sibling files.
static const std::filesystem::path& wtDir4279() {
    return wtScratchDir("wt-4279");
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
    std::string stem = unique_stem("bezel_4279lib_");
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
    std::string stem = unique_stem("bezel_4279app_");
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

// `Holder4279<T>` is GENERIC (the only shape that reaches `m->decl ==
// nullptr` for this walk — see file header) and satisfies all three of fix
// #4253's gating conditions: `sealed`, no explicit constructor (a class with
// no primary-ctor params and no `constructor` block has none —
// `hasAnyConstructor` inspects only `sema::ConstructorSymbol` members), and
// `readonly` on `P`, whose OWN declaration initializer is `new Box4279(...)`
// with a `+`-concatenation string argument (`isPlusOneStringProducer`, NOT a
// bare literal — a fresh heap allocation every construction, so a missed
// release is a REAL, measurable per-iteration leak rather than one that
// could be masked by literal interning).
const std::string kLibSrc4279 =
    "module Lib4279; "
    "public record Box4279(Value: string); "
    "public sealed class Holder4279<T> "
    "  public readonly property P: Box4279 := new Box4279(\"v-\" + 1.ToString()); "
    "end Holder4279; ";

// 200-iteration loop (LANE_RULES §30's amplification band), instantiating
// the imported generic `Holder4279<int>` fresh each iteration and disposing
// it at the `using` scope end — the Dispose call is exactly where this fix's
// new release walk runs.
const std::string kAppSrc4279 =
    "module App4279; "
    "import Lib4279; "
    "public class C4279 "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    var i: int := 0; "
    "    while i < 200 do "
    "      using var h := new Holder4279<int>(); "
    "      total := total + h.P.Value.Length; "
    "      i := i + 1; "
    "    end while; "
    "    return total; "
    "  end Main; "
    "end C4279; ";

}  // namespace

// ── HEADLINE — cross-module GENERIC-CLASS axis, ASan-instrumented, no leak ─
//
// FAIL-WITHOUT (measured, this send-back's own worktree — see file header):
// the pre-fix (`SEAM-DEBT`, no-fallback) version of `LlvmBackend_Disposal.cpp`
// leaks. PASS-WITH: no `Direct leak` / `Indirect leak` / `LeakSanitizer`
// strings at all. The exit-code oracle ("v-1" has Length 3, summed 200
// times = 600, truncated to 8 bits by WEXITSTATUS like every POSIX exit
// code, 600 % 256 == 88) is a REGRESSION-shape control, not the
// load-bearing half; the LSan-string absence is.
BEZEL_TEST(imported_generic_class_readonly_record_field_release_4279,
           imported_generic_holder_no_leak) {
    auto dir = wtDir4279();
    LibHandle libH = buildLib(dir, kLibSrc4279);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle appH = buildApp(dir, libH.objPath, kAppSrc4279, /*sanitize=*/true);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult rr = runCapture(appH.exePath);
    // Value-correctness control: "v-1" has Length 3, summed 200 times.
    BEZEL_EXPECT_EQ(rr.code % 256, (3 * 200) % 256);
    // Soundness oracle — the load-bearing half.
    BEZEL_EXPECT_TRUE(rr.out.find("LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Direct leak") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Indirect leak") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("AddressSanitizer") == std::string::npos);
}

}  // namespace bezel::codegen
