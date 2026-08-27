// Codegen — issue #3470 (§28.12 step 2, v0.283): an `await using var` bound to a
// USER class implementing `IAsyncDisposable` awaited `DisposeAsync()` and then
// emitted a bare `free(obj)`, so the object's `owned` fields and its base
// class's reclamation chain were never run. §28.12 step 2 states, verbatim:
//
//   "Physical reclamation runs once, after logical disposal has completed:
//    owned fields are disposed in reverse declaration order, the base class's
//    reclamation chains, and the heap block is released. This phase is the same
//    regardless of which logical-disposal path ran. For `await using`, the
//    runtime invokes physical reclamation as the second step of the implicit
//    `Dispose()` it issues after `DisposeAsync` returns; the destructor body
//    that would normally run in the synchronous path is skipped."
//
// The emitter treated "skip the destructor body" (step 1) as "skip the
// generated `Dispose()` entirely", which drops the other two thirds of step 2
// along with it. The plain-`using` twin — the same program with `await`
// deleted — reclaims correctly, which is the discriminator these rows use: the
// `await` spelling and the plain spelling must agree about a phase §28.12 says
// is the same for both.
//
// Measured on `bezelc --sanitize=address`, 200 executions of the issue's
// program: 14400 byte(s) leaked in 400 allocation(s) before the fix, and
// LeakSanitizer-clean after; the plain-`using` twin was clean on both.

#include "support/aot_support.hpp"   // runAot — the strict AOT oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// `Cell` is the OBSERVABLE for step 2's owned-field cascade: its destructor
// decrements a live count, so a surviving `Cell.n` is exactly "an owned field
// the cascade never disposed". A class with no owned state would have hidden
// the whole defect behind a correct exit code.
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";

// `Res` implements IAsyncDisposable, owns a Cell, and records which of the two
// §28.12 logical-disposal paths ran. Step 1 requires `DisposeAsync` to run and
// the destructor body NOT to: `dt` must stay 0 on the `await` spelling.
const char* kRes =
    "class Res: IAsyncDisposable public static da: int := 0; "
    "  public static dt: int := 0; "
    "  public owned _c: Cell; "
    "  public constructor() _c := new Cell(); end constructor; "
    "  public async method DisposeAsync(): Task Res.da := Res.da + 1; "
    "  end DisposeAsync; "
    "  destructor Res.dt := Res.dt + 1; end destructor; end Res; ";
}  // namespace

// §28.12 step 2: the owned-field cascade runs on the `await using` path.
// Observable = Cell.n*100 + Res.da*10 + Res.dt.
//   fixed:      0*100 + 1*10 + 0 = 10   (field reclaimed, DisposeAsync ran,
//                                        destructor body correctly skipped)
//   unfixed:  1*100 + 1*10 + 0 = 110  (the Cell is stranded — the bare free)
BEZEL_TEST(await_using_physical_reclamation_3470, owned_field_cascade_runs_28_12_step_2) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell + kRes +
        "class C "
        "  public static async method Run(): Task "
        "    await using var u := new Res(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await C.Run(); "
        "    return Cell.n * 100 + Res.da * 10 + Res.dt; "
        "  end Main; "
        "end C;"), 10);
}

// The DISCRIMINATOR row §28.12 step 2's "this phase is the same regardless of
// which logical-disposal path ran" names: the identical program with `await`
// deleted. Its observable is Cell.n*100 + Res.da*10 + Res.dt = 0 + 0 + 1 = 1
// (the plain path runs the destructor body and skips DisposeAsync, step 1's
// other half), and its Cell.n has always been 0. Both spellings must agree on
// the Cell.n digit — that agreement is the whole content of the issue.
BEZEL_TEST(await_using_physical_reclamation_3470, plain_using_twin_reclaims_identically) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell + kRes +
        "class C "
        "  public static async method Run(): Task "
        "    using var u := new Res(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await C.Run(); "
        "    return Cell.n * 100 + Res.da * 10 + Res.dt; "
        "  end Main; "
        "end C;"), 1);
}

// §28.12 step 2, "the base class's reclamation chains": a base level's `owned`
// field is reclaimed too. Observable = Cell.n (0 when both the derived Cell and
// the base Cell were disposed; 2 before the fix — nothing below the free ran).
BEZEL_TEST(await_using_physical_reclamation_3470, base_chain_owned_fields_reclaimed) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class Base public owned _b: Cell; "
        "  public constructor() _b := new Cell(); end constructor; end Base; "
        "class Der: Base, IAsyncDisposable public owned _d: Cell; "
        "  public constructor() base(); _d := new Cell(); end constructor; "
        "  public async method DisposeAsync(): Task end DisposeAsync; end Der; "
        "class C "
        "  public static async method Run(): Task "
        "    await using var u := new Der(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await C.Run(); "
        "    return Cell.n; "
        "  end Main; "
        "end C;"), 0);
}

// §28.12 step 1, the direction the fix must NOT overshoot in: physical
// reclamation is not licence to run the class's destructor body as well. A fix
// that simply routed the async path into the ordinary `Dispose()` would pass
// the cascade rows above and fail this one — `Res.dt` would be 1.
BEZEL_TEST(await_using_physical_reclamation_3470, destructor_body_still_skipped_28_12_step_1) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell + kRes +
        "class C "
        "  public static async method Run(): Task "
        "    await using var u := new Res(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await C.Run(); "
        "    return Res.dt; "  // 0 — DisposeAsync superseded the destructor
        "  end Main; "
        "end C;"), 0);
}

// LOOP-AMPLIFIED (200 iterations): a one-shot row can pass on both sides when
// the allocation is still reachable at exit, and it also cannot distinguish
// "reclaimed once" from "reclaimed once per some but not all executions". The
// live count must return to exactly 0 after 200 disposals, and the disposal
// count must be exactly 200 — the same amplification the ASan probe used to
// measure 14400 B / 400 allocations before the fix.
BEZEL_TEST(await_using_physical_reclamation_3470, amplified_200_reclamations_leave_no_live_cell) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell + kRes +
        "class C "
        "  public static async method Run(): Task "
        "    await using var u := new Res(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    for var i in 1..200 do await C.Run(); end; "
        "    return Cell.n * 3 + (200 - Res.da); "  // 0 iff both hold
        "  end Main; "
        "end C;"), 0);
}

// §28.12 step 2 on the ANONYMOUS binding (§17.13 / §28.10: `await using var _`
// is a full owning binding with the same scope-end disposal). Same cascade, a
// separate registration path in the emitter.
BEZEL_TEST(await_using_physical_reclamation_3470, anonymous_binding_cascades_too) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell + kRes +
        "class C "
        "  public static async method Run(): Task "
        "    await using var _ := new Res(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await C.Run(); "
        "    return Cell.n; "
        "  end Main; "
        "end C;"), 0);
}

// §28.12's synchronous-fallback path is untouched: on the UNWIND edge the
// emitter passes a null DisposeAsync (awaiting across an active unwind is a
// deferred increment), so the destructor runs, DisposeAsync does not, and the
// owned field is still reclaimed. Observable = Cell.n*100 + Res.da*10 + Res.dt
// = 0 + 0 + 1. This row guards the arm the fix routes AROUND.
BEZEL_TEST(await_using_physical_reclamation_3470, unwind_edge_sync_fallback_unchanged) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell + kRes +
        "class C "
        "  public static async method Run(): Task "
        "    await using var u := new Res(); "
        "    throw new InvalidOperationException(\"x\"); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    try "
        "      await C.Run(); "
        "    catch "
        "      Exception => Console.Write(\"\"), "
        "    end; "
        "    return Cell.n * 100 + Res.da * 10 + Res.dt; "
        "  end Main; "
        "end C;"), 1);
}
