// Codegen — async event RaiseAsync (#1176). Fix: `RaiseAsync` on an `async event`
// correctly invokes every subscribed handler in subscription order, awaiting each
// handler's Task before invoking the next (§24.5 / §30).
//
// Root cause: `collectInstanceStorage` (and the parallel predicates in
// `emitOwnedFieldDisposals` / `emitOwnFieldInit`) excluded `async event`
// declarations from the class struct layout, so async events had NO backing
// `ptr` slot for the `bz_event_source_t*`. As a result, `eventSourcePtr`
// returned nullptr for every async-event access, `tryLowerEventCall` returned
// nullopt for both `Subscribe` and `RaiseAsync`, and both fell through to
// generic method-call lowering which produced a no-op — handlers were never
// registered, never invoked, exit code was 0 instead of the expected value.
//
// Fix: async instance events are backed by the same `bz_event_source_t*` layout
// as synchronous instance events; the `!ed->isAsync` exclusion gates were removed
// from `collectInstanceStorage`, `emitOwnedFieldDisposals`, `emitOwnFieldInit`,
// and the ctor-cleanup owned-check.
//
// Spec derivation:
//   §24.5: "The asynchronous form `RaiseAsync` returns a `Task` that completes
//           when every handler's task completes; each handler's task is awaited
//           before the next is invoked."
//   §24.5: "If an asynchronous handler's task faults, subsequent handlers are
//           not invoked and the exception propagates from the awaited RaiseAsync."
//   §24.4: `Subscription.Dispose` removes the handler; subsequent raises are
//           no-ops for that subscriber.
//   §30.1 / §30.2: `await E.RaiseAsync(payload)` in an async method suspends
//           at the await point and resumes when the returned Task completes.
//   v0.154 spec.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// Shared infrastructure: a payload record `Ev(N: int)`, an accumulator class
// `Box`, and a class `Src` that holds `public async event E: Ev` and exposes
// an `async method Fire(n): Task` that raises the event. Handlers are async
// lambdas `async (ev) => do box.Add(ev.N); end` — they complete synchronously
// (no inner `await`) so the outer task is always already-done when returned.
const char* kPrelude =
    "module M; "
    "record Ev(N: int); "
    "class Box "
    "  public V: int := 0; "
    "  public method Add(x: int) V := V + x; end Add; "
    "end Box; "
    "class Src "
    "  public async event E: Ev; "
    "  public async method Fire(n: int): Task "
    "    await E.RaiseAsync(new Ev(n)); "
    "  end Fire; "
    "end Src; ";

// Main body wrapper: async Main returns Task<int> (the JIT harness awaits it
// via bz_task_await_sync internally, reading the i32 exit code from the task).
std::string prog(const std::string& mainBody) {
    return std::string(kPrelude) +
           "class C "
           "  public static async method Main(): Task<int> " + mainBody +
           "  end Main; "
           "end C;";
}

}  // namespace

// §24.5 / #1176: a SINGLE subscribed async handler is invoked and its
// side-effect (box.V += 3) is visible after awaiting Fire. Pre-fix: rc == 0.
BEZEL_TEST(codegen_async_event_1176, single_handler_invoked_spec_24_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var src := new Src(); "
        "var box := new Box(); "
        "var sub := src.E.Subscribe(async lambda(ev) => do box.Add(ev.N); end); "
        "await src.Fire(3); "
        "sub.Dispose(); "
        "return box.V; "   // expect 3
    )), 3);
}

// §24.5 / #1176: TWO sequential Fire calls, single handler — both payloads
// accumulate (3 + 4 = 7). Mirrors the issue's stated repro (Fire(3)/Fire(4)).
BEZEL_TEST(codegen_async_event_1176, two_fires_accumulate_spec_24_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var src := new Src(); "
        "var box := new Box(); "
        "var sub := src.E.Subscribe(async lambda(ev) => do box.Add(ev.N); end); "
        "await src.Fire(3); "
        "await src.Fire(4); "
        "sub.Dispose(); "
        "return box.V; "   // expect 7
    )), 7);
}

// §24.5 / #1176: TWO subscribers — both receive the payload on a single Fire.
// Proves handlers are called in subscription order (sub1 first, then sub2).
BEZEL_TEST(codegen_async_event_1176, two_handlers_both_invoked_spec_24_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var src := new Src(); "
        "var box := new Box(); "
        "var sub1 := src.E.Subscribe(async lambda(ev) => do box.Add(ev.N); end); "
        "var sub2 := src.E.Subscribe(async lambda(ev) => do box.Add(ev.N * 10); end); "
        "await src.Fire(2); "
        "sub1.Dispose(); sub2.Dispose(); "
        "return box.V; "   // expect 2 + 20 = 22
    )), 22);
}

// §24.5 / #1176: zero subscribers — RaiseAsync is a no-op; the returned Task
// completes immediately (success) and the exit code reflects the un-mutated box.
BEZEL_TEST(codegen_async_event_1176, zero_handlers_noop_spec_24_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var src := new Src(); "
        "var box := new Box(); "
        "await src.Fire(5); "     // no subscribers → no-op
        "return box.V; "           // expect 0
    )), 0);
}

// §24.4 / #1176: Dispose unsubscribes — a Fire after Dispose is a no-op
// and does not invoke the removed handler.
BEZEL_TEST(codegen_async_event_1176, dispose_unsubscribes_spec_24_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "var src := new Src(); "
        "var box := new Box(); "
        "var sub := src.E.Subscribe(async lambda(ev) => do box.Add(ev.N); end); "
        "await src.Fire(3); "   // box.V = 3
        "sub.Dispose(); "       // unsubscribe
        "await src.Fire(4); "   // handler gone — no-op
        "return box.V; "        // expect 3
    )), 3);
}

// =========================================================================
// §24.5 / §30.13 / #1377: continuation-chaining tests
//
// These tests require handlers that GENUINELY SUSPEND (await Task.Delay).
// Under the prior bz_task_await_sync implementation the domain drain thread
// would block waiting for a step that can only be scheduled ON that same
// thread → deadlock.  Under the continuation-chain fix, each handler step
// is scheduled non-blockingly via bz_task_then, so the domain drain loop
// can process both the original await-suspension and the chained resumes.
// =========================================================================

// §24.5 / §30.13 / #1377: a GENUINELY SUSPENDING handler (awaits Task.Delay)
// must not deadlock.  Pre-fix: deadlocks.  Post-fix: completes with box.V = 5.
BEZEL_TEST(codegen_async_event_1176, suspending_handler_no_deadlock_spec_24_5_30_13) {
    BEZEL_EXPECT_EQ(run(prog(
        "var src := new Src(); "
        "var box := new Box(); "
        "var sub := src.E.Subscribe(async lambda(ev) => do "
        "  await Task.Delay(1); "
        "  box.Add(ev.N); "
        "end); "
        "await src.Fire(5); "
        "sub.Dispose(); "
        "return box.V; "    // expect 5
    )), 5);
}

// §24.5 / #1377: two suspending handlers run in subscription order.
// handler 1 adds N; handler 2 adds N*10.  Sequential chaining means both
// complete before the outer task resolves: 3 + 30 = 33.
BEZEL_TEST(codegen_async_event_1176, two_suspending_handlers_order_spec_24_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var src := new Src(); "
        "var box := new Box(); "
        "var sub1 := src.E.Subscribe(async lambda(ev) => do "
        "  await Task.Delay(1); "
        "  box.Add(ev.N); "
        "end); "
        "var sub2 := src.E.Subscribe(async lambda(ev) => do "
        "  await Task.Delay(1); "
        "  box.Add(ev.N * 10); "
        "end); "
        "await src.Fire(3); "
        "sub1.Dispose(); sub2.Dispose(); "
        "return box.V; "    // expect 3 + 30 = 33
    )), 33);
}

// §24.5 / #1377: first-error-wins — a suspending-then-throwing first handler
// faults RaiseAsync; the second handler must NOT execute.
// box.V stays 0 (sub2 never ran); caught = 1 → return 1.
BEZEL_TEST(codegen_async_event_1176, first_error_wins_suspending_spec_24_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var src := new Src(); "
        "var box := new Box(); "
        "var sub1 := src.E.Subscribe(async lambda(ev) => do "
        "  await Task.Delay(1); "
        "  throw new InvalidOperationException(); "
        "end); "
        "var sub2 := src.E.Subscribe(async lambda(ev) => do "
        "  box.Add(100); "
        "end); "
        "var caught := 0; "
        "try "
        "  await src.Fire(1); "
        "catch Exception => caught := 1; "
        "end; "
        "sub1.Dispose(); sub2.Dispose(); "
        "return box.V * 10 + caught; "  // expect 0*10 + 1 = 1
    )), 1);
}
