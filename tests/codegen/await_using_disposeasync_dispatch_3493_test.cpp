// Codegen — issue #3493 (§28.12 step 1, v0.283): `registerUsingDisposal`'s
// generic `await using` arm resolved the `DisposeAsync` to call STATICALLY,
// from the DECLARED type of the binding, so
//
//     await using var u: Base := new Der();
//
// emitted `bz$Base$DisposeAsync(u)` — Base's body, unconditionally — and Der's
// asynchronous cleanup never ran. §28.12 step 1 states, verbatim:
//
//   "For `await using`, the awaited `DisposeAsync()` performs whatever
//    asynchronous cleanup the class defines; the destructor body (if any) is
//    not also run, because `DisposeAsync` supersedes it."
//
// "The class" is the runtime class: every other disposal path in the compiler
// already dispatches through the object's vtable, which is why the synchronous
// twin of the program above is correct and only the `await` spelling was wrong.
//
// #3493 filed this as blocked on "a dispatch point that does not exist" in the
// fixed vtable layout. That premise is REFUTED for this half, and these rows
// are the evidence: §12.2 forbids hiding, so a derived `DisposeAsync` can only
// exist as an `override` of a `virtual`/`abstract` base member — and `isVirtual`
// gives exactly those a vtable slot, already filled with the most-derived body
// (`vt$M.Base[6] = bz$M.Base$DisposeAsync`, `vt$M.Der[6] = bz$M.Der$DisposeAsync`
// in the emitted module for the program below).
//
// ⚠ NARROW. The physical-reclamation half of #3493 is NOT closed: the reclaim
// function is still keyed on the declared class, so a derived level's `owned`
// fields are still stranded. That residual is pinned by its own row at the
// bottom of this file, deliberately, so the row fails loudly the day it is
// fixed rather than silently encoding a leak forever.

#include "support/aot_support.hpp"   // runAot — the strict AOT oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// A two-level hierarchy whose `DisposeAsync` is `virtual`/`override`, with NO
// owned state — so these rows measure DISPATCH alone and cannot be confused
// with the reclamation residual, which has its own row. `bda`/`dda` record
// which asynchronous cleanup ran; `bdt`/`ddt` record destructor bodies, which
// step 1 says must not run on this path at all.
const char* kHier =
    "class Base: IAsyncDisposable public static bda: int := 0; "
    "  public static bdt: int := 0; "
    "  public async virtual method DisposeAsync(): Task "
    "    Base.bda := Base.bda + 1; end DisposeAsync; "
    "  destructor Base.bdt := Base.bdt + 1; end destructor; end Base; "
    "class Der: Base public static dda: int := 0; "
    "  public static ddt: int := 0; "
    "  public async override method DisposeAsync(): Task "
    "    Der.dda := Der.dda + 1; end DisposeAsync; "
    "  destructor Der.ddt := Der.ddt + 1; end destructor; end Der; ";
}  // namespace

// THE DEFECT. A `Der` reached through a `Base`-typed `await using var` must run
// DER's DisposeAsync. Observable = Base.bda * 10 + Der.dda.
//   fixed:   1    (Der's asynchronous cleanup ran)
//   unfixed: 10   (Base's ran — the statically resolved body)
BEZEL_TEST(await_using_disposeasync_dispatch_3493, derived_runs_through_base_typed_head) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kHier +
        "class C "
        "  public static async method Run(): Task "
        "    await using var u: Base := new Der(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await C.Run(); "
        "    return Base.bda * 10 + Der.dda; "
        "  end Main; "
        "end C;"), 1);
}

// The direction the fix must NOT overshoot in: dispatching the LOGICAL cleanup
// dynamically is not licence to run a destructor body as well. §28.12 step 1
// supersedes Der's, and fix #3470's per-class reading (issue 3492) leaves
// Base's to the reclamation chain — which, for a `Base`-declared head, is
// `bz$Base$DisposeReclaim`, whose whole point is that it omits Base's own body.
// Observable = Base.bdt * 10 + Der.ddt = 0 on both compilers; this row exists to
// FAIL a fix that reached for the vtable's `Dispose` slot (slot 2) instead of
// the `DisposeAsync` slot, which would have made it 11.
BEZEL_TEST(await_using_disposeasync_dispatch_3493, dispatch_runs_no_destructor_body) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kHier +
        "class C "
        "  public static async method Run(): Task "
        "    await using var u: Base := new Der(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await C.Run(); "
        "    return Base.bdt * 10 + Der.ddt; "
        "  end Main; "
        "end C;"), 0);
}

// CONTROL — an exactly-typed head is unchanged (it was already correct, and a
// dispatch that resolved anything else here would be a regression).
// Observable = Base.bda * 10 + Der.dda = 1 on both compilers.
BEZEL_TEST(await_using_disposeasync_dispatch_3493, exact_typed_head_unchanged) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kHier +
        "class C "
        "  public static async method Run(): Task "
        "    await using var u: Der := new Der(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await C.Run(); "
        "    return Base.bda * 10 + Der.dda; "
        "  end Main; "
        "end C;"), 1);
}

// CONTROL — the OTHER direction of the same head. A `Base` in a `Base`-typed
// binding must still reach BASE's DisposeAsync: a dispatch that always found
// the most-derived override in the program (rather than in the object) would
// pass the defect row above and fail this one.
// Observable = Base.bda * 10 + Der.dda = 10 on both compilers.
BEZEL_TEST(await_using_disposeasync_dispatch_3493, base_typed_head_holding_a_base_runs_base) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kHier +
        "class C "
        "  public static async method Run(): Task "
        "    await using var u: Base := new Base(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await C.Run(); "
        "    return Base.bda * 10 + Der.dda; "
        "  end Main; "
        "end C;"), 10);
}

// CONTROL — the `disposeAsyncVSlot == -1` arm. A NON-virtual `DisposeAsync` has
// no vtable slot, and §12.2's no-hiding rule means no subclass can declare one
// of its own, so the direct call is already the only implementation reachable
// and the emitter must keep making it. Observable = Solo.da = 1.
BEZEL_TEST(await_using_disposeasync_dispatch_3493, non_virtual_head_keeps_the_direct_call) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Solo: IAsyncDisposable public static da: int := 0; "
        "  public async method DisposeAsync(): Task Solo.da := Solo.da + 1; "
        "  end DisposeAsync; end Solo; "
        "class C "
        "  public static async method Run(): Task "
        "    await using var u := new Solo(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await C.Run(); "
        "    return Solo.da; "
        "  end Main; "
        "end C;"), 1);
}

// LOOP-AMPLIFIED (200 iterations): a one-shot dispatch row cannot distinguish
// "always reaches the runtime class" from "reaches it on the first iteration".
// Observable = Base.bda * 10000 + (200 - Der.dda) — 0 iff every one of the 200
// disposals reached Der's body and none reached Base's.
BEZEL_TEST(await_using_disposeasync_dispatch_3493, amplified_200_derived_dispatches) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kHier +
        "class C "
        "  public static async method Run(): Task "
        "    await using var u: Base := new Der(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    for var i in 1..200 do await C.Run(); end; "
        "    return Base.bda * 10000 + (200 - Der.dda); "
        "  end Main; "
        "end C;"), 0);
}

// ⚠ THE KNOWN RESIDUAL OF #3493, PINNED AS SUCH — this row asserts a DEFECT,
// not a rule, and is the only row here that does. The physical reclamation is
// still keyed on the DECLARED class (`disposerReclaimFns_[Base]`), so through a
// `Base`-typed head Der's `owned` field is never disposed even though
// `bz$M.Der$DisposeReclaim` exists in the module. Measured: 112 bytes leaked in
// 3 allocations under `bezelc --sanitize=address` on both the pre- and
// post-dispatch-fix compiler — the dispatch fix does not widen or narrow it.
// Observable = Cell.n: 1 (Der's Cell stranded) while 0 would mean the residual
// is closed. WHEN THIS ROW FAILS WITH 0, the residual has been fixed: update it
// to expect 0 and close the issue rather than "repairing" the fixture.
BEZEL_TEST(await_using_disposeasync_dispatch_3493, residual_derived_owned_field_still_stranded) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cell public static n: int := 0; "
        "  public constructor() Cell.n:=Cell.n+1; end constructor; "
        "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; "
        "class Base: IAsyncDisposable "
        "  public async virtual method DisposeAsync(): Task end DisposeAsync; "
        "end Base; "
        "class Der: Base public owned _d: Cell; "
        "  public constructor() _d := new Cell(); end constructor; "
        "  public async override method DisposeAsync(): Task end DisposeAsync; "
        "end Der; "
        "class C "
        "  public static async method Run(): Task "
        "    await using var u: Base := new Der(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await C.Run(); "
        "    return Cell.n; "
        "  end Main; "
        "end C;"), 1);
}
