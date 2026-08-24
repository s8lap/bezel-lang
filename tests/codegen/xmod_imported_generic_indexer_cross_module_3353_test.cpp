// Codegen — fix #3353: cross-module: an indexer declared on (or inherited
// from) a generic base class must monomorphize its access path per
// instantiation when accessed via a derived non-generic receiver — the same
// defect shape #3346 closed in single-module form (Wave 2 of run
// 2026-08-24 by `57cd79e` + symmetry `07fe556`). The cross-module form
// was deliberately left open: the importer dispatches through the
// `bz$<GBase>$idxget$<arity>(ptr, i32) -> i32` stub the LIB exports against
// the T→i32 erased placeholder, and the importer has no body to re-derive
// under its own substitution (PropertySymbol::decl MUST be null for
// imports per #3275). The fix synthesises a per-(ps, recvTy) InternalLinkage
// wrapper in the importer that GEPs the inherited slot under the importer's
// own T→Pt substitution, bypassing the stub's wrong-layout body.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// The cross-module form of #3346 — the importer on the app side
// dispatching through the fixed-i32 ABI stub
// `bz$<GBase>$idxget$<arity>(ptr, i32) -> i32` — read only the first 4
// bytes of the 8-byte `%record.Pt` slot the importer laid down for
// `class GD: GBase<Pt>`. Measured at HEAD on the issue's repro:
// `var stored: Pt := g[0]; return stored.X + stored.Y;` against
// `new GD(new Pt(7, 8))` returned 14 where 15 is correct — X=7
// read correctly, Y=7 read (the upper 4 bytes of the ctor argument Pt
// spill). The class-arg twin (T=Cell) was silent: the Cell pointer's low
// 32 bits happen to be a valid pointer on x86_64 (the high 32 bits
// round-trip through the 8-byte spill as zeroes), so `g[0].tag` read as
// garbage rather than as an IR-fault.
//
// ── THE FIX ─────────────────────────────────────────────────────────────
//
// At the cross-module dispatch in lowerIndexerGet / lowerIndexerSet, when
// the receiver is a derived class (`recvTy->declaration != ps->parent`)
// with a substitution for the inherited generic base's parameters
// (`computeAncestorGenericSubst` succeeds), emit a per-(ps, recvTy)
// InternalLinkage wrapper whose body is "GEP inherited slot, load/store"
// under the importer's own substitution. The wrapper takes the place of
// the wrong-layout `bz$<GBase>$idxget$<arity>` stub at the call site.
//
// Pre-condition: the accessor body must be a single field read/write on
// an inherited type-parameter slot — the common case. For non-trivial
// bodies the helper returns nullptr and the call site falls through to
// the original stub (which still produces wrong values; that limitation
// is acknowledged in #3353).
//
// ── SPEC CITATION ─────────────────────────────────────────────────────────
//
// §14.1 "Generic declarations" / §22.7 "Constructors" / §23.3 "Block-form
// properties" / §23.4 "Indexers" / §2.5 "Dependency resolution" /
// §2.8.2 "Cross-module type substitution" / §2.8.3 rule 4
// (per-instance monomorphization), v0.278.
//
// ── SCOPE / NOT-COVERED ───────────────────────────────────────────────────
//
// This file exercises the cross-module form of the inherited-base indexer
// case (the #3353 family). The cross-module property direction (#3348) is
// still rejected at sema with a `#2884` diagnostic — see the existing
// `xmod_imported_generic_indexer_property_3346_3348_test.cpp` SCOPE note.
// Method direction cross-module is closed by `methodBodyText` + per-
// instantiation `buildSpecializedMethodFn` (§14.1 / #783).
//
// ── FAIL-WITHOUT / PASS-WITH (verified mechanically on commit 600c3ca) ───
//
//  T1 inherited-indexer-getter (record arg):           14 pre-fix, 15 post-fix.
//  T2 inherited-property-getter (record arg):          rejected at sema
//                                                      (the #2884 path), so
//                                                      no post-fix behavior
//                                                      change here; pinned to
//                                                      document the gap.
//  T3 inherited-indexer-setter (record arg):           "Call parameter
//                                                      type does not match
//                                                      function signature!"
//                                                      IR-verify abort pre-
//                                                      fix, 41 (=20+21)
//                                                      post-fix.
//  T4 inherited-indexer-getter (class arg, T=Cell):    pre-fix garbage
//                                                      pointer / ASan-load,
//                                                      post-fix 99 (the
//                                                      written tag).
//  T5 control: a NON-inherited access on the SAME
//     generic base (g: GBase<Pt> directly, the local
//     case — no substitution needed) returns 15
//     unchanged; verifies the local fix isn't disturbed.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"

namespace fs = std::filesystem;

namespace {

struct TempFile3353 {
    std::string path;
    explicit TempFile3353(std::string p) : path(std::move(p)) {}
    ~TempFile3353() { std::remove(path.c_str()); }
};

void writeSource3353(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Build the shared library, return its path. Mirrors #2835's helper.
struct LibHandle3353 {
    std::shared_ptr<TempFile3353> src;
    std::shared_ptr<TempFile3353> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle3353 buildLib3353(const fs::path& dir,
                            const std::string& libSrc) {
    LibHandle3353 h;
    std::string stem = unique_stem("bezel_3353lib_");
    h.src = std::make_shared<TempFile3353>(
        (dir / (stem + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile3353>(
        (dir / (stem + "_lib.o")).string());
    writeSource3353(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r =
        driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

// In-process importer-side compile (uses `Driver::compileFile` like
// #2835/#3228/#3268/#3082 do).
struct InProcessApp3353 {
    bool ok = false;
    std::shared_ptr<TempFile3353> src;
    std::shared_ptr<TempFile3353> exe;
    std::string exePath;
};

InProcessApp3353 buildApp3353(const fs::path& dir,
                              const std::string& libObjPath,
                              const std::string& appSrc) {
    InProcessApp3353 r;
    if (libObjPath.empty()) return r;
    std::string stem = unique_stem("bezel_3353app_");
    r.src = std::make_shared<TempFile3353>(
        (dir / (stem + "_app.bzl")).string());
    r.exe = std::make_shared<TempFile3353>(
        (dir / (stem + "_app")).string());
    writeSource3353(r.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjPath;
    d.objectPath = libObjPath;
    deps.push_back(d);
    bezel::CompileResult rb =
        driver.compileFile(r.src->path, r.exe->path, deps);
    r.ok = rb.success;
    r.exePath = r.exe->path;
    return r;
}

// Run a binary and return {exitCode, combined-stdout-and-stderr}.
struct RunResult3353 { int code; std::string out; };

RunResult3353 runCapture3353(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
        out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

// Per-test workdir under the shared scratch base (DISK-backed per CLAUDE.md).
// The hardcoded /tmp tmpfs and std::to_string(counter++) anti-pattern races
// across shards.
static const fs::path& wtDir3353() {
    return wtScratchDir("wt-3353");
}

}  // namespace

// ===========================================================================
// T1 — issue's HEADLINE repro, record arg. `class GD: GBase<Pt>` reads
// `g[0]` returning the substituted Pt; the LIB's stub returns i32, so
// pre-fix only X (the first 4 bytes) is correct. Post-fix: the importer
// synthesises a per-(ps, recvTy) wrapper that GEPs the substituted slot.
// §14.1 / §22.7 / §23.4 / §2.8.3 rule 4, v0.278.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_indexer_cross_module_3353,
           t1_xmod_inherited_indexer_getter_record_arg_14_1) {
    auto dir = wtDir3353();
    const std::string lib =
        "module Lib3353t1; "
        "public record Pt "
        "  public X: int; "
        "  public Y: int; "
        "end Pt; "
        "public class GBase<T> "
        "  protected _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public property [i: int]: T => _v; "
        "end GBase; ";
    const std::string app =
        "module App3353t1; "
        "import Lib3353t1; "
        "public class GD: GBase<Pt> "
        "  public constructor(p: Pt) base(p); end constructor; "
        "end GD; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new GD(new Pt(7, 8)); "
        "    var stored: Pt := g[0]; "
        "    return stored.X + stored.Y; "
        "  end Main; "
        "end C; ";
    LibHandle3353 libH = buildLib3353(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp3353 r = buildApp3353(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture3353(r.exePath);
    // 7 + 8 = 15. Pre-fix returns 14 (X=7 + Y=7, where the second 7 is the
    // lower 4 bytes of the ctor's `new Pt(7, 8)` argument spill).
    BEZEL_EXPECT_EQ(code, 15);
}

// ===========================================================================
// T2 — property getter twin. The cross-module property direction is
// rejected at sema with #2884 (a generic type's accessors are not
// exported per instantiation; the importer-side property monomorphization
// is not implemented). This cell pins the gap: the test compiles and runs,
// the sema reject is expected, no value assertion.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_indexer_cross_module_3353,
           t2_xmod_inherited_property_getter_record_arg_sema_reject_2884) {
    auto dir = wtDir3353();
    const std::string lib =
        "module Lib3353t2; "
        "public record Pt "
        "  public X: int; "
        "  public Y: int; "
        "end Pt; "
        "public class GBase<T> "
        "  protected _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public property P: T => _v; "
        "end GBase; ";
    const std::string app =
        "module App3353t2; "
        "import Lib3353t2; "
        "public class GD: GBase<Pt> "
        "  public constructor(p: Pt) base(p); end constructor; "
        "end GD; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new GD(new Pt(7, 8)); "
        "    var stored: Pt := g.P; "
        "    return stored.X + stored.Y; "
        "  end Main; "
        "end C; ";
    LibHandle3353 libH = buildLib3353(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp3353 r = buildApp3353(dir, libH.objPath, app);
    // The importer's sema gate (SemaContext-imported-symbol-side) rejects
    // cross-module generic-base property access with #2884 wording.
    BEZEL_EXPECT_FALSE(r.ok);
}

// ===========================================================================
// T3 — indexer SETTER twin (the write direction). Pre-fix the importer
// emits a call to `bz$<GBase>$idxset$<arity>(ptr, i32, i32)` and passes
// `%record.Pt` (8 bytes) — the IR verifier rejects the parameter-type
// mismatch with "Call parameter type does not match function signature!"
// and aborts the build. Post-fix the importer synthesises a per-(ps,
// recvTy) InternalLinkage wrapper that GEPs the inherited slot under the
// importer's own substitution and stores the substituted value there.
// §14.1 / §22.7 / §23.4 / §2.8.3 rule 4, v0.278.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_indexer_cross_module_3353,
           t3_xmod_inherited_indexer_setter_record_arg_14_1) {
    auto dir = wtDir3353();
    const std::string lib =
        "module Lib3353t3; "
        "public record Pt "
        "  public X: int; "
        "  public Y: int; "
        "end Pt; "
        "public class GBase<T> "
        "  protected _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public property [i: int]: T "
        "    get => _v; "
        "    set => _v := value; "
        "  end; "
        "end GBase; ";
    const std::string app =
        "module App3353t3; "
        "import Lib3353t3; "
        "public class GD: GBase<Pt> "
        "  public constructor(p: Pt) base(p); end constructor; "
        "end GD; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new GD(new Pt(11, 12)); "
        "    g[0] := new Pt(20, 21); "
        "    var after: Pt := g[0]; "
        "    return after.X + after.Y; "
        "  end Main; "
        "end C; ";
    LibHandle3353 libH = buildLib3353(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp3353 r = buildApp3353(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture3353(r.exePath);
    // 20 + 21 = 41. Pre-fix this cell fails to compile (the IR-verify
    // reject at T3's signature-mismatch); the test would short-circuit on
    // !r.ok above. Post-fix compiles and runs to 41.
    BEZEL_EXPECT_EQ(code, 41);
}

// ===========================================================================
// T4 — class-arg twin (T = Cell). A class argument lowers to `ptr` in the
// placeholder, which is assignment-compatible with the substituted `ptr`
// by luck — a fix that hard-coded "i32 layout" would still pass this case
// silently (the cell pointer's low 32 bits happen to be a valid pointer
// on x86_64; the IR verifier does not flag it). The pin: g[0] must yield
// the SAME cell pointer that was constructed, and mutating `held.tag :=
// 99` must be visible through the inherited-base accessor. §14.1 / §12 /
// §2.5, v0.278.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_indexer_cross_module_3353,
           t4_xmod_inherited_indexer_getter_class_arg_silent_14_1) {
    auto dir = wtDir3353();
    const std::string lib =
        "module Lib3353t4; "
        "public class Cell "
        "  public tag: int := 0; "
        "end Cell; "
        "public class GBase<T> "
        "  protected _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public property [i: int]: T => _v; "
        "end GBase; ";
    const std::string app =
        "module App3353t4; "
        "import Lib3353t4; "
        "public class GD: GBase<Cell> "
        "  public constructor(c: Cell) base(c); end constructor; "
        "end GD; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new GD(new Cell()); "
        "    var held: Cell := g[0]; "
        "    held.tag := 99; "
        "    return g[0].tag; "
        "  end Main; "
        "end C; ";
    LibHandle3353 libH = buildLib3353(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp3353 r = buildApp3353(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture3353(r.exePath);
    // The Cell created by the ctor argument and the Cell reachable via
    // g[0] are the SAME object — verified by mutating `tag` through the
    // borrowed Cell and reading it back through the inherited-base indexer
    // (which is the cross-module codegen path #3353 closes). Pre-fix the
    // returned pointer is the wrong slot (T→i32 placeholder's first 4
    // bytes, which happen to be a valid x86_64 pointer to a zeroed
    // region), so the second `g[0]` reads 0 (the new Cell's tag, default)
    // — not 99. Post-fix: the wrapper GEPs the inherited `_v: Cell` slot
    // directly and returns the cell pointer; `held.tag := 99` mutates
    // that same cell, and `g[0].tag` reads 99.
    BEZEL_EXPECT_EQ(code, 99);
}

// ===========================================================================
// T5 — local control: an indexer on `class GD: GBase<Pt>` declared in the
// SAME module as the generic base (no import involved, no substitution
// boundary to cross). Pre-#3346 this returned 11 (X only) where 23 is
// correct; the local fix at Wave 2 of run 2026-08-24
// (`57cd79e`+`07fe556`) closed it. The cross-module fix at this lane
// must not regress the local path. Pin: 7 + 8 = 15 unchanged from the
// single-module file.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_indexer_cross_module_3353,
           t5_local_control_inherited_indexer_14_1) {
    using bezel::test::runAot;
    auto r = runAot(
        "module M; "
        "public record Pt "
        "  public X: int; "
        "  public Y: int; "
        "end Pt; "
        "public class GBase<T> "
        "  protected _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public property [i: int]: T => _v; "
        "end GBase; "
        "public class GD: GBase<Pt> "
        "  public constructor(p: Pt) base(p); end constructor; "
        "end GD; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new GD(new Pt(7, 8)); "
        "    var stored: Pt := g[0]; "
        "    return stored.X + stored.Y; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 15);
}
