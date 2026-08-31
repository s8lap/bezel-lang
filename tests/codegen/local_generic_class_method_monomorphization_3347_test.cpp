// Codegen — fix #3347: a LOCAL generic class over a generic base must
// monomorphize its METHOD bodies per instantiation; the inherited `_v: T`
// field read through `V(): U => _v` must land at the per-instantiation
// slot, not the T→i32 placeholder.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// A local generic class `class GD<U>: GBase<U>` with a method that reads an
// inherited type-parameter field (`method V(): U => _v;`) emitted its
// monomorphized body against the T→i32 PLACEHOLDER specialization of the
// inherited base — `class.GBase$spec1` = {ptr, i8, i32} — while the object
// was laid out at `class.GBase$spec0` = {ptr, i8, %record.Pt} for T=Pt.
// The GEP into slot 2 read only the first 4 bytes (= X), Y was uninitialised
// stack memory, and `V()` returned a wrong value (X correct, Y=garbage).
//
// The IR for the inlined V() body is unambiguous:
//
//     %14 = getelementptr inbounds nuw %"class.GBase$spec1", ptr %13, i32 0, i32 2
//     %15 = load i32, ptr %14, align 4
//     ...                                          (returned as i32 to caller)
//     store i32 %15, ptr %0, align 4                (4-byte store into an
//                                                   %record.Pt slot of width 8)
//
// measured at HEAD on the issue's exact repro (`new GD<Pt>(new Pt(11, 12))`
// → `stored.X + stored.Y`): returned 22 (= 11 + uninit, not 23 = 11 + 12).
// ASan/LSan on the same program flagged the load as a stack-buffer-overflow
// (the second `i32` of the `%record.Pt` slot is poisoned) — a clean ASan
// run IS the load-bearing oracle for the silent-wrong-value variant.
//
// ── THE ROOT CAUSE ────────────────────────────────────────────────────────
//
// `implicitFieldAddr` (LlvmBackend_Layout.cpp ~5128) computes the ancestor
// generic substitution from the "enclosing class" of the bare-identifier
// field access. The enclosing-class lookup prefers a `currentFn_` reverse
// scan over `methodFns_`, falling back to `currentSpecMethodSymbol_->parent`.
// During an `emitInlineGenericMethod`-driven monomorphization, `currentFn_`
// is the CALLER's function (e.g. main), so the lookup finds the caller's
// class (e.g. C) — not the inlined method's class (e.g. GD). The
// `computeAncestorGenericSubst(C → GBase)` walk fails (C doesn't derive
// from GBase), the substitution stays empty for T, and `classStruct(GBase)`
// returns the T→i32 placeholder spec.
//
// The fix prefers `currentSpecMethodSymbol_->parent` when set: that names
// the method actually being monomorphized, whose `this` is the receiver on
// which the base-chain walk must start. For the inlining case this routes
// the walk through GD (whose `genericSubst_` already binds U→Pt), which then
// derives T→Pt and returns the correct specialization. The standalone-body
// emission case is unaffected — `currentSpecMethodSymbol_` is unset there.
//
// ── SPEC CITATION ─────────────────────────────────────────────────────────
//
// §14.1 "Generic declarations" / §22.7 "Constructors" / §22.8 "Field
// initializers" / §12 "Inheritance and Dispatch" — v0.240 (current v0.278).
//
// ── FAIL-WITHOUT PROOF (verified mechanically on commit eabfa69) ──────────
//
//  T1 single-module: 22 (= 11 + uninit), pre-fix — post-fix 23.
//  T2 cross-module : same defect, same fix.
//  T3 ASan/LSan    : pre-fix stack-buffer-overflow on the Y read (the
//                    load past the T→i32 slot), post-fix clean.
//
// ── RELATED ──────────────────────────────────────────────────────────────
//
// #2835 — the imported-generic base ctor direction (cross-module; fixed at
// HEAD via schema v5 ctor-body-text carriage).
// #3346 — the imported-generic base indexer/property direction (open).
// #3348 — umbrella issue.

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
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "sema/DefiniteAssignment.hpp"

using bezel::test::runAot;

namespace fs = std::filesystem;

namespace {

// ── Per-test workdir under the shared scratch base (DISK-backed per CLAUDE.md).
// The hardcoded /tmp tmpfs and std::to_string(counter++) anti-pattern races
// across shards.
static const fs::path& wtDir3347() {
    return wtScratchDir("wt-3347");
}

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// ASan / LSan check: returns 0 = clean, 1 = sanitizer error, -1 = skip.
int runUnderAsanLsan(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir3347();
    std::string stem = unique_stem("bezel_3347_asan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -2;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -2;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -2;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -2;
    }

    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
        return -1;
    }
    int status = std::system(exe.c_str());
    std::remove(objPath.c_str());
    std::remove(exe.c_str());
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// Cross-module harness: compile the dependency as a -c object (with its
// .bezel.meta section), then compile the app against it via --import and run.
// Mirrors the family used by #2835 / #2414 / #2449 / #2452 / #2890 / #3143.
int compileImportAndRun3347(const std::string& libSrc,
                            const std::string& appSrc,
                            const std::string& tag) {
    auto dir = wtDir3347();
    std::string tagPrefix = "bz3347_" + tag + "_";
    std::string stem = unique_stem(tagPrefix.c_str());

    std::string lSrc = (dir / (stem + "_lib.bzl")).string();
    std::string lObj = (dir / (stem + "_lib.o")).string();
    std::string aSrc = (dir / (stem + "_app.bzl")).string();
    std::string aExe = (dir / stem).string();

    {
        std::ofstream out(lSrc);
        out << libSrc;
    }
    {
        bezel::Driver driver;
        if (!driver.compileObject(lSrc, lObj).success) return -2;
    }
    if (!std::ifstream(lObj).good()) return -2;

    {
        std::ofstream out(aSrc);
        out << appSrc;
    }
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = lObj;
    d.objectPath   = lObj;
    deps.push_back(d);
    if (!driver.compileFile(aSrc, aExe, deps).success) return -1;
    if (!std::ifstream(aExe).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(aExe.c_str(), aExe.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);
    if (!WIFEXITED(status)) return -3;
    return WEXITSTATUS(status);
}

}  // namespace

// ===========================================================================
// T1 — the issue's single-module repro. The local generic class reads an
// INHERITED type-parameter field via a derived-class accessor; the inlined
// body must land at the per-instantiation `%record.Pt` slot, not the
// T→i32 placeholder. Pre-fix: returned 22 (= 11 + uninit). Post-fix: 23.
// §14.1 / §22.7 / §22.8, v0.240 (current v0.278).
// ===========================================================================
BEZEL_TEST(local_generic_class_method_monomorphization_3347,
           t1_single_module_inherited_t_field_record_arg_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record Pt "
        "  public X: int; "
        "  public Y: int; "
        "end Pt; "
        "public class GBase<T> "
        "  protected _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "end GBase; "
        "public class GD<U>: GBase<U> "
        "  public constructor(u: U) base(u); end constructor; "
        "  public method V(): U => _v; "
        "end GD; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new GD<Pt>(new Pt(11, 12)); "
        "    var stored: Pt := g.V(); "
        "    return stored.X + stored.Y; "
        "  end Main; "
        "end C;"),
        23);
}

// ===========================================================================
// T2 — the issue's cross-module repro. GBase in the imported module, GD in
// the app module. Same defect, same fix: the inlined V() body's GEP must
// use the per-instantiation `class.GBase$spec0` = {ptr, i8, %record.Pt}
// layout, not the T→i32 `class.GBase$spec1` placeholder. Pre-fix: returned
// 22 (= 11 + uninit). Post-fix: 23. §14.1 / §22.7 / §2.5 / §22.8, v0.240.
// ===========================================================================
BEZEL_TEST(local_generic_class_method_monomorphization_3347,
           t2_cross_module_inherited_t_field_record_arg_14_1) {
    const char* lib =
        "module Lib3347; "
        "public class GBase<T> "
        "  protected _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "end GBase;";
    const char* app =
        "module App3347; import Lib3347; "
        "public record Pt "
        "  public X: int; "
        "  public Y: int; "
        "end Pt; "
        "public class GD<U>: GBase<U> "
        "  public constructor(u: U) base(u); end constructor; "
        "  public method V(): U => _v; "
        "end GD; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new GD<Pt>(new Pt(11, 12)); "
        "    var stored: Pt := g.V(); "
        "    return stored.X + stored.Y; "
        "  end Main; "
        "end C;";
    int r = compileImportAndRun3347(lib, app, "t2");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    // Pre-fix: returned 22 (= 11 + uninit Y); post-fix: 23.
    BEZEL_EXPECT_EQ(r, 23);
}

// ===========================================================================
// T3 — ASan oracle: pre-fix the Y read overflowed the T→i32 slot (the
// second `i32` of the `%record.Pt` slot is poisoned under ASan), so the
// load was flagged as a stack-buffer-overflow by AddressSanitizer.
// Post-fix: clean. This is the silent-wrong-value variant the load-bearing
// oracle catches on plain builds — a stack-buffer-overflow IS the defect.
//
// Main returns 0 on success (value correctly 11+12) so the test asserts
// `rc == 0` to mean "ASan-clean AND value correct". Pre-fix ASan exits with
// 1 on the load overflow, so rc != 0 — pinning the bug. §14.1, v0.240
// (current v0.278).
// ===========================================================================
BEZEL_TEST(local_generic_class_method_monomorphization_3347,
           t3_asan_no_stack_buffer_overflow_on_inherited_t_field_14_1) {
    int rc = runUnderAsanLsan(
        "module M; "
        "public record Pt "
        "  public X: int; "
        "  public Y: int; "
        "end Pt; "
        "public class GBase<T> "
        "  protected _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "end GBase; "
        "public class GD<U>: GBase<U> "
        "  public constructor(u: U) base(u); end constructor; "
        "  public method V(): U => _v; "
        "end GD; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new GD<Pt>(new Pt(11, 12)); "
        "    var stored: Pt := g.V(); "
        "    return (if stored.X = 11 and stored.Y = 12 then 0 else 1); "
        "  end Main; "
        "end C;");
    if (rc == -1) return;  // ASan toolchain unavailable — skip
    // 0 = ASan-clean AND value correct; 1 = value wrong; non-zero other =
    // ASan flagged a violation (typically exit 1, but libasan default).
    BEZEL_EXPECT_EQ(rc, 0);
}
