// Codegen — a static-event handler subscribed INSIDE an async method observes
// its captured-state mutations (#1502, §24.4 / §30.1 / §22.13, v0.180).
//
// Root cause: emitFunctionBody routes an async method to emitAsyncMethod and
// returns BEFORE the `isMain(ms)` entry-point static-init prologue that a
// synchronous Main runs. That prologue is the ONLY place a static event's
// backing EventSource<T> is allocated (§22.13 / §24.1 #477 B6 — a program-wide
// static root, not a per-type lazy ensureinit), so an async Main never created
// the `bz$<Class>$<evt>$evt` source. Subscribe and Raise then both operated on
// the null global: the handler was never registered, every Raise silently
// dropped it, and the handler's captured-state mutations were never observed —
// even for a Raise before any `await`. A synchronous Main was unaffected
// (static_event3.bz printed 12; the async static_event5.bz printed 0).
//
// Differential (from the issue): a directly-invoked closure mutating a captured
// local in the SAME async Main worked (closure_async.bz printed 5), proving the
// defect was specific to the static-event Subscribe/Raise path × the async
// entry's missing static-init, not the capture-cell machinery (#694).
//
// Fix: factor the Main static-init prologue into emitMainStaticInitPrologue and
// run it (plus the declaring type's ensureinit) from emitAsyncMethod's entry,
// before the state machine is driven — matching the synchronous path exactly.
//
// Spec derivation:
//   §24.4:  external code subscribes through `.Subscribe()` and the handler is
//           invoked on every `.Raise()`.
//   §22.13: a static event's backing source is a static owned field allocated
//           at program start (the single static root), so subscribe/raise on a
//           static event reaches a real shared source.
//   §30.1:  async-method locals; the synchronous prefix runs before the first
//           await, so a Raise before any await must already observe mutations.
//   §27.5 / §28.9: a handler lambda's captured local/instance is shared storage;
//           mutations through the handler are visible to the enclosing method.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// A payload record `Tick(N)`, an accumulator `Box`, and a `Bus` carrying a
// synchronous STATIC event `Ticked` plus a static `Fire` that raises it. This
// is the issue's static_event3/static_event5 shape.
const char* kPrelude =
    "module M; "
    "record Tick(N: int); "
    "class Box "
    "  public V: int := 0; "
    "  public method Add(x: int) V := V + x; end Add; "
    "end Box; "
    "class Bus "
    "  public static event Ticked: Tick; "                             // §24.1
    "  public static method Fire(n: int) Ticked.Raise(new Tick(n)); "  // §24.5
    "  end Fire; "
    "  public static async method Nop(): Task end Nop; "  // completes immediately
    "end Bus; ";

// async Main returning Task<int>; the JIT harness awaits it and reads the i32.
std::string asyncMain(const std::string& body) {
    return std::string(kPrelude) +
           "class C public static async method Main(): Task<int> " + body +
           " end Main; end C;";
}

// synchronous Main baseline (guards the prologue-extraction refactor).
std::string syncMain(const std::string& body) {
    return std::string(kPrelude) +
           "class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// §24.4 / §30.1 / §22.13 (#1502, core repro): a capturing handler subscribed to
// a static event INSIDE an async Main observes its captured-local mutations on
// each Raise. This is static_event5.bz reduced to an exit code. Pre-fix: 0.
BEZEL_TEST(codegen_static_event_async_1502, async_subscribe_captured_local_observes_mutations) {
    BEZEL_EXPECT_EQ(run(asyncMain(
        "var total := 0; "
        "using var sub := Bus.Ticked.Subscribe((e) => do total := total + e.N; end); "
        "Bus.Fire(5); "
        "Bus.Fire(7); "
        "return total; "     // expect 12
    )), 12);
}

// §30.1 (synchronous prefix): a Raise BEFORE any await must already observe the
// mutation — the bug lost it even here, proving it was the missing static-event
// source at entry, not an await spill/restore fault.
BEZEL_TEST(codegen_static_event_async_1502, async_subscribe_mutation_visible_before_await) {
    BEZEL_EXPECT_EQ(run(asyncMain(
        "var total := 0; "
        "using var sub := Bus.Ticked.Subscribe((e) => do total := total + e.N; end); "
        "Bus.Fire(5); "                 // before any await
        "var before := total; "
        "await Bus.Nop(); "
        "return before; "               // expect 5
    )), 5);
}

// §30.1 / §30.2: the captured-local mutation accumulates ACROSS an await — a
// Raise in the synchronous prefix and a Raise after resume both land in the
// shared cell.
BEZEL_TEST(codegen_static_event_async_1502, async_subscribe_accumulates_across_await) {
    BEZEL_EXPECT_EQ(run(asyncMain(
        "var total := 0; "
        "using var sub := Bus.Ticked.Subscribe((e) => do total := total + e.N; end); "
        "Bus.Fire(5); "
        "await Bus.Nop(); "
        "Bus.Fire(7); "
        "return total; "     // expect 12
    )), 12);
}

// §27.5 / §28.9: the handler mutates a captured CLASS INSTANCE field (not a
// local) subscribed inside an async Main. This is the issue's probe_obj shape.
// Pre-fix: 0.
BEZEL_TEST(codegen_static_event_async_1502, async_subscribe_captured_object_observes_mutations) {
    BEZEL_EXPECT_EQ(run(asyncMain(
        "using var box := new Box(); "
        "using var sub := Bus.Ticked.Subscribe((e) => do box.Add(e.N); end); "
        "Bus.Fire(5); "
        "Bus.Fire(7); "
        "return box.V; "     // expect 12
    )), 12);
}

// §24.4: TWO capturing handlers subscribed inside an async Main both fire on a
// single Raise, in subscription order.
BEZEL_TEST(codegen_static_event_async_1502, async_subscribe_two_handlers_both_fire) {
    BEZEL_EXPECT_EQ(run(asyncMain(
        "using var box := new Box(); "
        "using var s1 := Bus.Ticked.Subscribe((e) => do box.Add(e.N); end); "
        "using var s2 := Bus.Ticked.Subscribe((e) => do box.Add(e.N * 10); end); "
        "Bus.Fire(2); "
        "return box.V; "     // expect 2 + 20 = 22
    )), 22);
}

// §24.4 / §22.13 (regression guard): the SAME subscription in a synchronous
// Main still works after the prologue was factored into
// emitMainStaticInitPrologue — static_event3.bz.
BEZEL_TEST(codegen_static_event_async_1502, sync_subscribe_captured_local_baseline) {
    BEZEL_EXPECT_EQ(run(syncMain(
        "var total := 0; "
        "using var sub := Bus.Ticked.Subscribe((e) => do total := total + e.N; end); "
        "Bus.Fire(5); "
        "Bus.Fire(7); "
        "return total; "     // expect 12
    )), 12);
}

// §27.5 / §30.1 (regression guard): a directly-invoked closure mutating a
// captured local in an async Main still works — closure_async.bz. This is the
// differential control that isolates the fix to the static-event path.
BEZEL_TEST(codegen_static_event_async_1502, async_direct_closure_captured_local_control) {
    BEZEL_EXPECT_EQ(run(asyncMain(
        "var total := 0; "
        "var bump := (n: int) => do total := total + n; end; "
        "bump(5); "
        "return total; "     // expect 5
    )), 5);
}
