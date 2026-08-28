// Codegen — issue #3494 (§28.12 step 2's FAULT EDGE, v0.283): a `DisposeAsync`
// that completes BY THROWING re-raised inside `emitAwaitSuspend`'s own resume
// block, before any of the physical reclamation `emitAsyncDisposeAwaitAndFree`
// emits after the await. The object was therefore reclaimed by the enclosing
// UNWIND pass instead — which takes §28.12's synchronous fallback
// (`emitDisposals` passes a null `disposeAsyncFn` when `isUnwind`), i.e. the
// full `Dispose()`, destructor body included.
//
// §28.12 step 2 (v0.283), verbatim, on that edge:
//
//   "This includes the fault edge: when `DisposeAsync` completes by throwing,
//    logical disposal is over — the runtime performs physical reclamation, then
//    the exception propagates from the await."
//
// and step 1, verbatim:
//
//   "For `await using`, the awaited `DisposeAsync()` performs whatever
//    asynchronous cleanup the class defines; the destructor body (if any) is
//    not also run, because `DisposeAsync` supersedes it."
//
// So the destructor body must NOT run on the fault edge either — the sentence
// is about which logical-disposal path ran, not about which edge it left by.
//
// Measured on the issue's program before the fix (100 throw/catch cycles):
// `DA` x100, `DTOR` x100, `CAUGHT` x100. After: `DA` x100, no `DTOR`.

#include "support/aot_support.hpp"   // runAot — the strict AOT oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// `Cell` carries TWO counters, not one, because this fix moves a reclamation
// between two mechanisms and both directions are live hazards: `n` (live
// instances) catches an UNDER-release — a fault edge that reclaims nothing —
// and `d` (destructor entries) catches an OVER-release, the double cascade that
// would follow if the fault-edge reclamation ran AND the unwind pass still
// found the slot populated. A single live-count observable cannot tell those
// apart: both a correct single disposal and a double one end at n == 0.
const char* kCell =
    "class Cell public static n: int := 0; public static d: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; Cell.d:=Cell.d+1; end destructor; end Cell; ";

// `Res` throws from `DisposeAsync`. `da` counts the asynchronous cleanup, `dt`
// the destructor body §28.12 step 1 says it supersedes.
const char* kRes =
    "class Res: IAsyncDisposable public static da: int := 0; "
    "  public static dt: int := 0; "
    "  public owned _c: Cell; "
    "  public constructor() _c := new Cell(); end constructor; "
    "  public async method DisposeAsync(): Task Res.da := Res.da + 1; "
    "    throw new InvalidOperationException(\"boom\"); "
    "  end DisposeAsync; "
    "  destructor Res.dt := Res.dt + 1; end destructor; end Res; ";

// A `Run` whose `await using` binding's DisposeAsync throws, and a `Main` that
// catches the propagated fault. Shared by the rows below.
const char* kRunThrows =
    "class C "
    "  public static async method Run(): Task "
    "    await using var u := new Res(); "
    "  end Run; ";
}  // namespace

// §28.12 step 1 — THE DEFECT. On the fault edge the destructor body ran,
// because the unwind pass reclaimed the object through the full synchronous
// `Dispose()`. Observable = Res.dt.
//   fixed:   0   (DisposeAsync superseded it, on this edge too)
//   unfixed: 1   (the unwind pass's synchronous fallback ran it)
BEZEL_TEST(await_using_fault_edge_3494, throwing_disposeasync_skips_destructor_body_28_12_step_1) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell + kRes + kRunThrows +
        "  public static async method Main(): Task<int> "
        "    try await C.Run(); catch Exception => Console.Write(\"\"), end; "
        "    return Res.dt; "
        "  end Main; "
        "end C;"), 0);
}

// §28.12 step 2 on the fault edge — "the runtime performs physical
// reclamation". PARITY ROW, LABELLED AS ONE: it passes on BOTH compilers,
// because before the fix the unwind pass's synchronous `Dispose()` did reclaim
// the owned subtree (wrongly, but completely). It is here to pin the half of
// the sequence the fix must NOT drop while moving the other half: a fix that
// only nulled the slot early — stopping the destructor body — would strand the
// Cell and fail exactly this row. Observable = Cell.n.
BEZEL_TEST(await_using_fault_edge_3494, throwing_disposeasync_still_reclaims_owned_fields) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell + kRes + kRunThrows +
        "  public static async method Main(): Task<int> "
        "    try await C.Run(); catch Exception => Console.Write(\"\"), end; "
        "    return Cell.n; "
        "  end Main; "
        "end C;"), 0);
}

// §28.12 step 2's "physical reclamation runs ONCE" — the OVER-RELEASE
// direction, which LeakSanitizer is blind to and a live-count-only observable
// hides. `Cell.d` counts destructor ENTRIES over 200 fault edges: exactly 200
// means the cascade ran once per object. A fault-edge reclamation that left the
// slot populated for the unwind pass would reach 400 (or abort on the double
// free of the block). Observable = Cell.n * 10000 + (200 - Cell.d).
BEZEL_TEST(await_using_fault_edge_3494, amplified_200_faults_reclaim_exactly_once) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell + kRes + kRunThrows +
        "  public static async method Main(): Task<int> "
        "    for var i in 1..200 do "
        "      try await C.Run(); catch Exception => Console.Write(\"\"), end; "
        "    end; "
        "    return Cell.n * 10000 + (200 - Cell.d); "
        "  end Main; "
        "end C;"), 0);
}

// LOOP-AMPLIFIED step 1: 200 fault edges, not one. A single execution can pass
// for the wrong reason (an allocation still reachable at exit, a counter read
// before the disposal it names); 200 also distinguishes "never runs the
// destructor body" from "runs it on some edges". Observable =
// Res.dt * 10000 + (200 - Res.da) — 0 iff the destructor body never ran and
// DisposeAsync ran on every one of the 200 iterations.
BEZEL_TEST(await_using_fault_edge_3494, amplified_200_faults_run_disposeasync_only) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell + kRes + kRunThrows +
        "  public static async method Main(): Task<int> "
        "    for var i in 1..200 do "
        "      try await C.Run(); catch Exception => Console.Write(\"\"), end; "
        "    end; "
        "    return Res.dt * 10000 + (200 - Res.da); "
        "  end Main; "
        "end C;"), 0);
}

// "...THEN the exception propagates from the await." The reclamation must not
// swallow the fault: the original exception still reaches the enclosing
// handler, and it is still the one `DisposeAsync` threw. Asserted by the arm
// that MATCHED and by the MESSAGE it carried, on stdout — not by presence, and
// not by the exit code (a wrong-arm match and a swallowed fault would both be
// exit 0). The `WRONGARM` / `NOFAULT` alternatives make each failure mode name
// itself in the diff.
BEZEL_TEST(await_using_fault_edge_3494, fault_still_propagates_from_the_await) {
    BEZEL_EXPECT_EQ(runAotOut(
        std::string("module M; ") + kCell + kRes + kRunThrows +
        "  public static async method Main(): Task<int> "
        "    try "
        "      await C.Run(); "
        "      Console.Write(\"NOFAULT\"); "
        "    catch "
        "      var ex: InvalidOperationException => Console.Write(ex.Message), "
        "      Exception => Console.Write(\"WRONGARM\"), "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"), "boom");
}

// CONTROL — the NORMAL edge is untouched. Same class with a DisposeAsync that
// does NOT throw: DisposeAsync runs, the destructor body does not, the owned
// field is reclaimed. Observable = Cell.n * 100 + Res.da * 10 + Res.dt = 10.
BEZEL_TEST(await_using_fault_edge_3494, non_throwing_disposeasync_path_unchanged) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class Ok: IAsyncDisposable public static da: int := 0; "
        "  public static dt: int := 0; "
        "  public owned _c: Cell; "
        "  public constructor() _c := new Cell(); end constructor; "
        "  public async method DisposeAsync(): Task Ok.da := Ok.da + 1; "
        "  end DisposeAsync; "
        "  destructor Ok.dt := Ok.dt + 1; end destructor; end Ok; "
        "class C "
        "  public static async method Run(): Task "
        "    await using var u := new Ok(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await C.Run(); "
        "    return Cell.n * 100 + Ok.da * 10 + Ok.dt; "
        "  end Main; "
        "end C;"), 10);
}

// CONTROL — the UNWIND edge (a throw in the BODY, not in DisposeAsync) keeps
// §28.12's synchronous fallback: awaiting across an already-active unwind is a
// deferred increment, so the destructor runs and DisposeAsync does not. This is
// the arm the fix moves the slot-null in front of, so it is the row that would
// catch the move over-reaching. Observable =
// Cell.n * 100 + Ok.da * 10 + Ok.dt = 1.
BEZEL_TEST(await_using_fault_edge_3494, body_throw_unwind_edge_sync_fallback_unchanged) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class Ok: IAsyncDisposable public static da: int := 0; "
        "  public static dt: int := 0; "
        "  public owned _c: Cell; "
        "  public constructor() _c := new Cell(); end constructor; "
        "  public async method DisposeAsync(): Task Ok.da := Ok.da + 1; "
        "  end DisposeAsync; "
        "  destructor Ok.dt := Ok.dt + 1; end destructor; end Ok; "
        "class C "
        "  public static async method Run(): Task "
        "    await using var u := new Ok(); "
        "    throw new InvalidOperationException(\"x\"); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    try await C.Run(); catch Exception => Console.Write(\"\"), end; "
        "    return Cell.n * 100 + Ok.da * 10 + Ok.dt; "
        "  end Main; "
        "end C;"), 1);
}

// §28.12 step 2's "the base class's reclamation chains", on the FAULT edge. The
// per-class reading fix #3470 recorded (issue 3492) says the base level's
// cleanup is NOT superseded — only the class whose `DisposeAsync` ran has its
// destructor body skipped — so `Base`'s destructor body must still run and its
// owned field must still be reclaimed. Observable =
// Cell.n * 100 + Der.dt * 10 + Base.bdt = 1: both Cells reclaimed, Der's
// destructor body skipped, Base's run once.
BEZEL_TEST(await_using_fault_edge_3494, base_chain_still_runs_on_the_fault_edge) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class Base public static bdt: int := 0; public owned _b: Cell; "
        "  public constructor() _b := new Cell(); end constructor; "
        "  destructor Base.bdt := Base.bdt + 1; end destructor; end Base; "
        "class Der: Base, IAsyncDisposable public static dt: int := 0; "
        "  public owned _d: Cell; "
        "  public constructor() base(); _d := new Cell(); end constructor; "
        "  public async method DisposeAsync(): Task "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end DisposeAsync; "
        "  destructor Der.dt := Der.dt + 1; end destructor; end Der; "
        "class C "
        "  public static async method Run(): Task "
        "    await using var u := new Der(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    try await C.Run(); catch Exception => Console.Write(\"\"), end; "
        "    return Cell.n * 100 + Der.dt * 10 + Base.bdt; "
        "  end Main; "
        "end C;"), 1);
}
