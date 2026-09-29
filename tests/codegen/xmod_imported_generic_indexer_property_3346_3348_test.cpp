// Codegen — fix #3346/#3348: an indexer or property declared on (or
// inherited from) a generic base class must monomorphize its access path
// per instantiation when accessed via a derived non-generic receiver. The
// read must land at the per-instantiation slot, not the T→i32 erased
// placeholder specialization of the inherited base.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// The local-derived-receiver shape `class GD<U>: GBase<U>` (or
// `class GD: GBase<Pt>`) accessing `g[i]` / `g.P` where the indexer/property
// is declared on `GBase<T>` triggered the inline-body path. The inline body
// was lowered against `GBase`'s T→i32 erased placeholder structure
// (`class.GBase$specN = {ptr, i8, i32}`) while `g`'s storage was laid out
// for the substituted type (e.g. `class.GBase$spec0 = {ptr, i8, %record.Pt}`
// for T=Pt). The implicit GEP read only the first 4 bytes of the
// substituted `_v: T` slot — silent corruption, X correct, Y garbage (or
// X correct, Y undefined under ASan).
//
// Measured at HEAD on the issue's repro (`new GD<Pt>(new Pt(11, 12))`
// + `var stored: Pt := g[0]; return stored.X + stored.Y`): returned 11 (X
// only) where 23 (X+Y) is correct. ASan/LSan flag the load as a
// stack-buffer-overflow (the second `i32` of the `%record.Pt` slot is
// poisoned) — ASan exit-1 is the load-bearing oracle for the silent-wrong-
// value variant.
//
// ── THE ROOT CAUSE ────────────────────────────────────────────────────────
//
// `setupReceiverGenericSubst` (LlvmBackend_Calls.cpp:811) is what
// `lowerIndexerGet`'s inline-body path calls to install the per-instantiation
// generic substitution before the indexer getter body is lowered. Its
// early-out gate (`recvTy->declaration != parent || recvTy->genericArgs.empty()`)
// returned the SAVED empty substitution when the receiver's STATIC type was
// a derived non-generic class (e.g. `g: GD: GBase<Pt>`, `recvTy->declaration
// = GD != GBase`). The inlined indexer body's `_v` read then walked
// `classStruct(GBase)` with NO substitution active — i32 erased layout, not
// the substituted record layout. The PROPERTY twin in
// LlvmBackend_Exprs.cpp had an INLINED COPY of the same gate at line 5348,
// which fired the same way; the NAMED property returned 22 where 23 was
// correct.
//
// The METHOD direction (#3347, closed) used `computeAncestorGenericSubst`
// to walk the receiver's base chain and install the inherited base's
// substitution. The same helper is exactly what was missing on the
// indexer / property access paths.
//
// ── THE FIX ─────────────────────────────────────────────────────────────
//
// Extend `setupReceiverGenericSubst` (LlvmBackend_Calls.cpp:811) so that,
// on the early-out branch, it calls `computeAncestorGenericSubst` with
// `parent` as the ancestor and, if that returns non-empty, installs the
// resulting substitution. Mirror the fix at the property inline-body site
// (LlvmBackend_Exprs.cpp:5387). Both walk the base chain from
// `recvTy->declaration` UP TO `parent` (the indexer/property declaring
// class), seed from the active substitution where `genericArgs.empty()`,
// and install the resolved `T → Pt`-style bindings.
//
// ── SPEC CITATION ─────────────────────────────────────────────────────────
//
// §14.1 "Generic declarations" / §23.3 "Block-form properties" /
// §23.4 "Indexers" / §2.8.3 rule 4 (per-instance monomorphization) /
// §2.5 "Dependency resolution" / §2.8.2 (module boundary), v0.278.
//
// ── SCOPE / NOT-COVERED ───────────────────────────────────────────────────
//
// This fix covers the LOCAL case (single-module, the receiver and the
// generic base are in the same compilation unit). The CROSS-MODULE form
// of #3346 — the IMPORTER's stub call site dispatching through a fixed
// `bz$<GBase>$idxget$<arity>(ptr, i32) -> i32` signature (the i32
// placeholder) — is NOT yet closed by this fix. The IMPORTER can compute
// the inherited-base layout under its own substitution, but cannot
// re-derive the indexer body without an `ast::PropertyDecl` (which
// PropertySymbol::decl == nullptr for imports per #3275). Filed as
// follow-up (FIXME-this-PR #3355). The T1-T6 cases here exercise the
// LOCAL form ONLY and FAIL if the importer is involved.
//
// ── FAIL-WITHOUT / PASS-WITH (verified mechanically on commit f693622) ────
//
//  T1 inherited-indexer-getter (single-module): 11 pre-fix, 23 post-fix.
//  T2 inherited-property-getter (single-module): 22 pre-fix, 23 post-fix.
//  T3 inherited-indexer-setter (single-module): 21+20=41.
//  T4 inherited-property-setter (single-module): 20+21=41.
//  T5 same with `class GD<U>: GBase<U>` shape (generic GD): same values.
//  T6 control: non-generic concrete derived class — already correct on
//     HEAD, no regression.
//  T7 ASan/LSan oracle: pre-fix poisoned-load, post-fix clean.
//  T8 save/restore symmetry regression guard for
//     `setupReceiverGenericSubst` (narrowed at pre-gate review): the
//     function now returns BOTH maps (`SavedReceiverSubst`) and all four
//     call sites restore both — see the §14.1 / #3346+#3348 narrow-fix
//     commit message and the T8 cell body comment for the latent-defect
//     analysis and the (intentional) non-fail-without-on-this-shape
//     rationale.
//
// ── RELATED ──────────────────────────────────────────────────────────────
//
// #3347 — local generic class METHOD monomorphization (inherited base).
// #2889 — local generic class FIELD-INITIALIZER layout on inherited
//         base (introduced `ancestorGenericSubstAdditions`).
// #585 — first inherited method-call fix introduced base-chain walking.
// #3275 — the cross-modulePropertySymbol::decl invariant; the importer-
//         side approach #3346 originally proposed was correctly REFUTED
//         here. Follow-up at FIXME-this-PR #3355.

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
static const fs::path& wtDir3346() {
    return wtScratchDir("wt-3346");
}

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// ASan / LSan check: returns 0 = clean, 1 = sanitizer error, -1 = skip.
//
// #3653: NOT converted to tests/support/run_under_asan.hpp. This copy
// predates #3633's per-cause return-code split entirely: every failure path
// (compile, link, and system() failure alike) still returns the OLD
// conflated -1, no ASAN_OPTIONS is set explicitly (leak detection depends on
// the sanitizer's own default rather than a documented detect_leaks=), and
// it returns the raw WEXITSTATUS(status) rather than a 0/1 verdict. Folding
// this in would silently change its skip/fail semantics.
int runUnderAsanLsan(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir3346();
    std::string stem = unique_stem("bezel_3346_asan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return bezel::test::asanBuildFailure("parse");
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return bezel::test::asanBuildFailure("sema");
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return bezel::test::asanBuildFailure("emitModule");
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    }

    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
        return -3;
    }
    int status = std::system(exe.c_str());
    std::remove(objPath.c_str());
    std::remove(exe.c_str());
    if (!WIFEXITED(status)) return -3;
    return WEXITSTATUS(status);
}

}  // namespace

// ===========================================================================
// T1 — the issue's core repro: `class GD: GBase<Pt>` (non-generic GD
// inheriting a concrete instantiation of a generic base), the indexer is
// declared on the generic base, accessed via `g[0]`. Pre-fix: 11 (only
// X read, Y dropped or garbage). Post-fix: 23. §14.1 / §23.4, v0.278.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_indexer_property_3346_3348,
           t1_inherited_indexer_getter_class_gd_pt_14_1) {
    BEZEL_EXPECT_EQ(run(
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
        "    var g := new GD(new Pt(11, 12)); "
        "    var stored: Pt := g[0]; "
        "    return stored.X + stored.Y; "
        "  end Main; "
        "end C;"),
        23);
}

// ===========================================================================
// T2 — same shape with NAMED PROPERTY `P: T => _v` instead of indexer.
// Pre-fix: 22 (X + uninit). Post-fix: 23. §14.1 / §23.3, v0.278.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_indexer_property_3346_3348,
           t2_inherited_property_getter_class_gd_pt_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record Pt "
        "  public X: int; "
        "  public Y: int; "
        "end Pt; "
        "public class GBase<T> "
        "  protected _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public property P: T => _v; "
        "end GBase; "
        "public class GD: GBase<Pt> "
        "  public constructor(p: Pt) base(p); end constructor; "
        "end GD; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new GD(new Pt(11, 12)); "
        "    var stored: Pt := g.P; "
        "    return stored.X + stored.Y; "
        "  end Main; "
        "end C;"),
        23);
}

// ===========================================================================
// T3 — inherited INDEXER setter (write-side). `g[0] := new Pt(20, 21)`
// stores a substituted-type record into the inherited slot, must take
// the per-instantiation offset. Pre-fix: stored only first 4 bytes (Y
// dropped). Post-fix: 20+21=41. §14.1 / §23.4, v0.278.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_indexer_property_3346_3348,
           t3_inherited_indexer_setter_class_gd_pt_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
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
        "end GBase; "
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
        "end C;"),
        41);
}

// ===========================================================================
// T4 — inherited PROPERTY setter. `g.P := new Pt(20, 21)` writes a
// substituted-type record into the inherited-base field. Pre-fix: stored
// only first 4 bytes. Post-fix: 20+21=41. §14.1 / §23.3, v0.278.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_indexer_property_3346_3348,
           t4_inherited_property_setter_class_gd_pt_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record Pt "
        "  public X: int; "
        "  public Y: int; "
        "end Pt; "
        "public class GBase<T> "
        "  protected _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public property P: T "
        "    get => _v; "
        "    set => _v := value; "
        "  end P; "
        "end GBase; "
        "public class GD: GBase<Pt> "
        "  public constructor(p: Pt) base(p); end constructor; "
        "end GD; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new GD(new Pt(11, 12)); "
        "    g.P := new Pt(20, 21); "
        "    var after: Pt := g.P; "
        "    return after.X + after.Y; "
        "  end Main; "
        "end C;"),
        41);
}

// ===========================================================================
// T5 — same with generic-derived class `class GD<U>: GBase<U>`. The
// inherited-base parameters bind transitively: GD's own `U = Pt` is
// already on the active substitution; the base walk must derive
// `T = Pt` from `class GD<U>: GBase<U>`. Pre-fix: same defect (Y
// dropped). Post-fix: 23 (and 41 for the setter twin, exercised as
// T5a/T5b by separate test cells). §14.1 / §23.4, v0.278.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_indexer_property_3346_3348,
           t5_inherited_indexer_getter_generic_gd_14_1) {
    BEZEL_EXPECT_EQ(run(
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
        "public class GD<U>: GBase<U> "
        "  public constructor(u: U) base(u); end constructor; "
        "end GD; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new GD<Pt>(new Pt(11, 12)); "
        "    var stored: Pt := g[0]; "
        "    return stored.X + stored.Y; "
        "  end Main; "
        "end C;"),
        23);
}

// ===========================================================================
// T6 — non-generic control: a plain class with no generic base must
// continue to work. Sanity check for `setupReceiverGenericSubst`'s
// `nGP == 0`-short-circuit removal path: with this local receiver,
// the function must NOT install any substitution and the program
// must still compile and run as before. §14.1 / §23.4.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_indexer_property_3346_3348,
           t6_non_generic_control_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Plain "
        "  public N: int := 7; "
        "  public constructor() end constructor; "
        "  public property [i: int]: int => N + i; "
        "end Plain; "
        "public class C "
        "  public static method Main(): int "
        "    var p := new Plain(); "
        "    return p[3]; "
        "  end Main; "
        "end C;"),
        10);
}



// ===========================================================================
// T7 — ASan oracle: pre-fix the Y read overflowed the T→i32 slot
// (the second `i32` of the `%record.Pt` slot is poisoned under ASan),
// so the load was flagged as a stack-buffer-overflow by
// AddressSanitizer. Post-fix: clean. Main returns 0 on success
// (X=11 and Y=12), so the test asserts `rc == 0` to mean
// "ASan-clean AND value correct". Pre-fix ASan exits with 1 on
// the load overflow, so rc != 0 — pinning the bug. §14.1, v0.278.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_indexer_property_3346_3348,
           t7_asan_no_stack_buffer_overflow_on_inherited_t_field_14_1) {
    int rc = runUnderAsanLsan(
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
        "    var g := new GD(new Pt(11, 12)); "
        "    var stored: Pt := g[0]; "
        "    return (if stored.X = 11 and stored.Y = 12 then 0 else 1); "
        "  end Main; "
        "end C;");
    if (rc == -1) return;  // ASan toolchain unavailable — skip
    // 0 = ASan-clean AND value correct; 1 = value wrong; non-zero other =
    // ASan flagged a violation (typically exit 1, but libasan default).
    BEZEL_EXPECT_EQ(rc, 0);
}

// ===========================================================================
// T8 — REGRESSION GUARD for the `setupReceiverGenericSubst` save/restore
// symmetry narrowed at pre-gate review of #3346/#3348. Two INDEPENDENT
// generic receivers in the same Main: the first read walks the inherited-
// base substitution for `g[0]` (the T→Pt binding the prior T1–T7 exercise);
// the second read walks a DIFFERENT generic receiver `q` whose own base
// binding is `Q→Qt` (a different generic receiver, NOT a `GBase<Pt>`-shaped
// one). Pre-fix the function returned only `savedSubst` (the LLVM-type
// map) for the caller to restore, leaving `genericSubstSemaTy_` polluted
// with the inner receiver's bindings — a symmetry break with the METHOD
// twin (Calls.cpp:~8980 / #3347 path) and the property inline-body twin
// (Exprs.cpp:~5360), both of which save and restore BOTH maps. The
// narrow fix changes the function signature to return BOTH maps (struct
// `SavedReceiverSubst`) and updates all four call sites to restore both,
// mirroring the proven-correct discipline of those twins.
//
// This cell does NOT deterministically fail pre-fix on the simple
// `g[0]; q[0]` interleaving — the second inherited-base call's
// setupReceiverGenericSubst re-overwrites the polluted semaTy map
// with its own bindings. The defect is LATENT and the pre-gate reviewer
// flagged it as a contract violation: the function's stated return
// semantic ("returning the previous map for the caller to restore") did
// not match its side-effect semantic (it modifies BOTH maps). The cell
// is here as a SHAPE GUARD: a future regression that re-introduces a
// one-sided restore would surface as either a wrong-value read or an
// ASan-flagged slot-overflow on the second receiver — and the
// second-inherited-receiver shape (T1–T7 only exercise ONE such
// receiver per Main) is the canonical shape any future symmetry-break
// would manifest in. §14.1, v0.278.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_indexer_property_3346_3348,
           t8_state_restoration_second_generic_receiver_in_same_main_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record Pt "
        "  public X: int; "
        "  public Y: int; "
        "end Pt; "
        "public record Qt "
        "  public A: int; "
        "end Qt; "
        "public class GBase<T> "
        "  protected _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public property [i: int]: T => _v; "
        "end GBase; "
        "public class GD: GBase<Pt> "
        "  public constructor(p: Pt) base(p); end constructor; "
        "end GD; "
        "public class QBase<Q> "
        "  protected _v: Q; "
        "  public constructor(v: Q) _v := v; end constructor; "
        "  public property [i: int]: Q => _v; "
        "end QBase; "
        "public class QD: QBase<Qt> "
        "  public constructor(q: Qt) base(q); end constructor; "
        "end QD; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new GD(new Pt(11, 12)); "
        "    var first: Pt := g[0]; "
        "    var q := new QD(new Qt(7)); "
        "    var second: Qt := q[0]; "
        "    return (if first.X = 11 and first.Y = 12 and second.A = 7 "
        "            then 0 else 1); "
        "  end Main; "
        "end C;"),
        0);
}
