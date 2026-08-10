// Codegen/runtime — §31.4 CauseChain reclaim through the PURE-RUNTIME
// vtable-Dispose-then-free sites (issue 2330, the named follow-up of issue
// 2989 / #1389 item 1).
//
// §31.4 (Standard exception types) / §30.9 (Exception handling in async
// methods) / §28.2 (exactly-once) / §28.9 (Runtime-managed types), v0.240.
//
// WHAT THE DEFECT IS. §31.4 (v0.240) states the chaining constructor's
// contract verbatim: "the `(message, cause)` constructor **borrows** `cause`
// for the duration of the call and copies its type name, message, stack-trace
// text, and its own `CauseChain` into the new exception's `CauseChain`". That
// copy is a heap `Array<ExceptionInfo>` the exception OWNS, so §30.9's fault
// rule — the fault exception "is disposed **exactly once, by the task that
// owns it, when the task is disposed**" — must reclaim it.
//
// The shared exception Dispose (`bz$exDisposeMsg`) deliberately does NOT free
// CauseChain: #811/#1389 recorded a use-after-free from doing so, because a
// still-borrowed exception's chain is read (as a cause's `nested` chain) after
// its Dispose has already dispatched on some path. The free therefore lives at
// the TERMINAL reclaim, immediately before the object's block goes away.
// Codegen emits that reclaim at `emitDisposeAndReclaim`; issue 2989 widened its
// gate to user subclasses. But a fault exception disposed through one of the
// runtime's own "virtual Dispose, then free()" sites never passes through
// codegen at all:
//   * bz_dispose_class_obj (bezel_runtime_concurrency.cpp) — the abandoned-
//     then-faulted task continuation registered by bz_task_fire_and_forget,
//     and the WhenAll/WhenAllOwned aggregate's per-input cleanup;
//   * bz_domain_spawn's DomainFaultedException wrap of the original fault.
// Those sites have no codegen-supplied table argument to gate the reclaim on,
// which is exactly why the original fix left them out — and they leaked.
//
// MEASURED ON THE PRE-FIX COMPILER (671726f), generated-code ASan/LSan, on the
// first cell's program:
//     Direct leak of 48 byte(s) in 1 object(s)   bz_array_alloc_count_n:38
//     Indirect leak of 24 byte(s) in 1 object(s) bz_array_alloc_count_n:40
//     SUMMARY: AddressSanitizer: 72 byte(s) leaked in 2 allocation(s)
// i.e. the snapshot array header + its element block, per faulted task. The
// exception OBJECT itself is disposed correctly on this path (#1601 item 4
// fixed that) — only its §31.4 snapshot leaked, which is what localizes the
// defect to the missing reclaim rather than to fault disposal generally.
//
// THE FIX. The tables are registered with the runtime once by the generated
// main prologue (`bz_exc_tables_init`, emitMainStaticInitPrologue), exactly as
// `bz_ser_exc_vtable_init` already hands over the SerializationException
// vtable; the runtime sites then reclaim through
// bz_exc_reclaim_causechain_registered before their free(). The reclaim STAYS
// terminal — it is not moved into the shared disposer, so the #811/#1389
// use-after-free constraint is respected.
//
// Oracle. LSan, attached by runAotChecked: the snapshot is a runtime
// `Array<ExceptionInfo>`, not a user class, so a destructor live-count cannot
// see it. A leak flips r.ran to false -> -999, distinct from any legal exit
// value. Pre-fix these cells return -999; post-fix 0.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// A leak / double-free / use-after-free flips r.ran to false -> -999.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ---------------------------------------------------------------------------
// §31.4 / §30.9 / issue 2330 — the canonical repro. `Boom` suspends before it
// throws, so the task is NOT complete when Task.FireAndForget takes it: that is
// bz_task_fire_and_forget's SLOW path, which registers the continuation that
// later disposes the settled fault via bz_dispose_class_obj. The thrown
// exception is built by the §31.4 `(message, cause)` constructor, so it owns a
// CauseChain snapshot. Pre-fix the object was disposed and freed but the
// snapshot was not -> 72 bytes leaked -> -999.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_fault_causechain_reclaim_2330,
           fire_and_forget_slow_path_chained_builtin_reclaims_causechain) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Boom(): Task "
        "    await Task.Delay(30); "               // suspend -> FaF slow path
        "    using var root := new Exception(\"root\"); "
        "    throw new Exception(\"wrap\", root); "  // §31.4 snapshot chaining
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    Task.FireAndForget(C.Boom()); "
        "    await Task.Delay(200); "              // let it settle + dispose
        "    return 0; "
        "  end Main; "
        "end C;"),
        0);
}

// ---------------------------------------------------------------------------
// §31.4 / issue 2330 composed with issue 2989 — the same runtime site, but the
// fault is a USER SUBCLASS chaining `base(msg, cause)`. Its vtable is vt$<C>,
// absent from the built-in vtable table, so reclaiming it depends on the
// DESCRIPTOR table (btd$Exception among its ancestors) reaching the runtime
// site through the registration. This is the cell that proves the registered
// table carries issue 2989's widened gate into the pure-runtime path, not just
// the vtable fast path.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_fault_causechain_reclaim_2330,
           fire_and_forget_slow_path_user_subclass_reclaims_causechain) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class MyEx: Exception "
        "  public constructor(m: string, c: Exception) base(m, c); end constructor; "
        "end MyEx; "
        "class C "
        "  public static async method Boom(): Task "
        "    await Task.Delay(30); "
        "    using var root := new Exception(\"root\"); "
        "    throw new MyEx(\"wrap\", root); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    Task.FireAndForget(C.Boom()); "
        "    await Task.Delay(200); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        0);
}

// ---------------------------------------------------------------------------
// §28.2 exactly-once — the OVER-RELEASE control. The reclaim now runs at a
// runtime site that also disposes ordinary (non-exception) class objects and
// unchained exceptions; neither may be harmed. `Keeper` has a destructor-backed
// live count and is disposed through the very same abandoned-task path as the
// fault above, and the fault here carries NO cause (a null CauseChain slot, the
// `!chain` early return). Returning Keeper.n == 0 means disposed exactly once —
// >0 would be a leak, <0 a double-free — and LSan still guards the whole run.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_fault_causechain_reclaim_2330,
           unchained_fault_and_plain_object_are_not_over_released) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class Keeper public static n: int := 0; "
        "  public constructor() Keeper.n := Keeper.n + 1; end constructor; "
        "  destructor Keeper.n := Keeper.n - 1; end destructor; "
        "end Keeper; "
        "class C "
        "  public static async method Boom(): Task "
        "    await Task.Delay(30); "
        "    using var k := new Keeper(); "        // disposed on unwind
        "    throw new Exception(\"plain\"); "     // no cause -> null slot
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    Task.FireAndForget(C.Boom()); "
        "    await Task.Delay(200); "
        "    return Keeper.n; "                    // 0 == exactly once
        "  end Main; "
        "end C;"),
        0);
}

// ---------------------------------------------------------------------------
// §31.4 — the reclaim must not run EARLY. The chain is still readable while the
// exception is live and being handled: this cell awaits the faulted task,
// catches the fault as a borrow, and reads `CauseChain.Count` inside the arm.
// If the snapshot were freed before the terminal reclaim (the #811/#1389
// use-after-free this fix deliberately does not reintroduce), ASan would report
// a heap-use-after-free on that read instead of a clean 1.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_fault_causechain_reclaim_2330,
           causechain_is_readable_in_the_catch_arm_before_reclaim) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Boom(): Task "
        "    await Task.Delay(20); "
        "    using var root := new Exception(\"root\"); "
        "    throw new Exception(\"wrap\", root); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var n := 0; "
        "    try "
        "      await C.Boom(); "
        "    catch var ex: Exception => n := ex.CauseChain.Count; "
        "    end; "
        "    return n; "                           // 1 == chain intact
        "  end Main; "
        "end C;"),
        1);
}
