// Codegen — member-call dispatch for `EventSource<T>` / `AsyncEventSource<T>`
// values held in a local/parameter (§6.5 / §24.3, #1610).
//
// Before #1610, an `EventSource<T>`/`AsyncEventSource<T>`-typed value bound to
// a local or parameter had NO member-call dispatch: every method call on it —
// valid (`Raise`, `Subscribe`, `RaiseAsync`) or bogus — silently compiled and
// NO-OP'd at runtime. Root cause: these builtin generics (declaration ==
// nullptr, matched by name) lacked an `rcn` dispatch block in
// `ExprTypecheck_Calls.cpp` (the `Channel<T>`/`Weak<T>` siblings each have
// one), and codegen's `tryLowerEventCall` only dispatched calls whose receiver
// expression was the literal event field (`eventSymbolOf`); the moment the
// event value passed through a typed local or parameter, both the sema typing
// and the codegen dispatch fell through with no effect.
//
// The fix (mirroring the `Channel<T>` pattern) adds an `rcn == "EventSource" ||
// rcn == "AsyncEventSource"` block that types each producer method (stdlib §6.5)
// and rejects unrecognized names, plus a codegen path in `tryLowerEventCall`
// that lowers these calls against the `bz_event_source_t*` the local/parameter
// holds (the same pointer a direct event access yields, via the event-as-value
// read).
//
// Spec authority:
//   - §6.5 ("member access"): a method call on a value dispatches to the
//     member's semantics; an unrecognized member is a compile error (not a
//     silent no-op).
//   - §24.3 ("the type of an event member"): inside the declaring class the
//     static type is `EventSource<T>` / `AsyncEventSource<T>`, where `Raise`,
//     `RaiseAsync`, `Add`, `Remove`, and `Subscribe` are all available; once a
//     value of that type is bound to a local/parameter, those members must
//     still dispatch.
//   - §24.5: `Raise`/`RaiseAsync` invoke each subscribed handler with the
//     payload; a raised event must reach subscribers.
//   - stdlib §6.5: `EventSource<T>.Subscribe`/`Raise`/`Add`/`Remove`,
//     `AsyncEventSource<T>.Subscribe`/`RaiseAsync`/`Add`/`Remove`.
//
// Each `EventSource<Ev>` value is obtained and raised INSIDE the declaring
// class — where it is the genuine producer-side face per §24.3 — through a
// typed parameter or local (the issue's own repro shape). Each case FAILS
// without the fix (the raise silently no-ops, so the handler never fires and
// `total` stays 0) and PASSES with it.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// The headline bug: `Raise` through a PARAMETER of type `EventSource<Ev>` — the
// issue's own repro shape (`Fire(es: EventSource<T>) es.Raise(...)`). Before the
// fix the handler never fired (exit 0); it must now fire (exit 5+7 = 12).
BEZEL_TEST(codegen_event_source_var_1610, param_raise_fires) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class Ev public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; end Ev; "
        "class Counter "
        "  public event Ticked: Ev; "
        "  public total: int := 0; "
        "  public method FireViaParam(es: EventSource<Ev>, n: int) "
        "    es.Raise(new Ev(n)); end FireViaParam; "
        "  public method Go(): int "
        "    using var s := Ticked.Subscribe(lambda(e) => do total := total + e.N; end); "
        "    FireViaParam(Ticked, 5); FireViaParam(Ticked, 7); return total; "
        "  end Go; "
        "end Counter; "
        "class C public static method Main(): int "
        "  using var c := new Counter(); return c.Go(); end Main; end C;"), 12);
}

// `Raise` through a LOCAL variable of type `EventSource<Ev>` (issue repro
// variant 2: `var es: EventSource<T> := Ticked; es.Raise(...)`).
BEZEL_TEST(codegen_event_source_var_1610, local_raise_fires) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class Ev public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; end Ev; "
        "class Counter "
        "  public event Ticked: Ev; "
        "  public total: int := 0; "
        "  public method FireViaLocal(n: int) "
        "    var es: EventSource<Ev> := Ticked; es.Raise(new Ev(n)); end FireViaLocal; "
        "  public method Go(): int "
        "    using var s := Ticked.Subscribe(lambda(e) => do total := total + e.N; end); "
        "    FireViaLocal(4); FireViaLocal(8); return total; "
        "  end Go; "
        "end Counter; "
        "class C public static method Main(): int "
        "  using var c := new Counter(); return c.Go(); end Main; end C;"), 12);
}

// `.Subscribe` on a stored `EventSource<Ev>` local (not just `.Raise`): the
// registration must take, so a subsequent `Raise` through the same local fires
// the handler.
BEZEL_TEST(codegen_event_source_var_1610, local_subscribe_fires) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class Ev public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; end Ev; "
        "class Counter "
        "  public event Ticked: Ev; "
        "  public total: int := 0; "
        "  public method Go(): int "
        "    var es: EventSource<Ev> := Ticked; "
        "    using var s := es.Subscribe(lambda(e) => do total := total + e.N; end); "
        "    es.Raise(new Ev(9)); return total; "
        "  end Go; "
        "end Counter; "
        "class C public static method Main(): int "
        "  using var c := new Counter(); return c.Go(); end Main; end C;"), 9);
}

// An UNTYPED handler lambda `(e) => …` through a stored `EventSource<Ev>` local
// — the payload type is seeded from the receiver's `EventSource<Ev>` so the
// param resolves and the body's `e.N` typechecks (#1610's sema seeding).
BEZEL_TEST(codegen_event_source_var_1610, local_untyped_lambda_fires) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class Ev public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; end Ev; "
        "class Counter "
        "  public event Ticked: Ev; "
        "  public total: int := 0; "
        "  public method Go(): int "
        "    var es: EventSource<Ev> := Ticked; "
        "    using var s := es.Subscribe(lambda(e) => do total := total + e.N * 2; end); "
        "    es.Raise(new Ev(6)); return total; "
        "  end Go; "
        "end Counter; "
        "class C public static method Main(): int "
        "  using var c := new Counter(); return c.Go(); end Main; end C;"), 12);
}

// The async sibling: `RaiseAsync` through a PARAMETER of type
// `AsyncEventSource<Ev>` (§24.5 / §30). Before the fix the async handler never
// ran; it must now fire.
BEZEL_TEST(codegen_event_source_var_1610, async_param_raiseasync_fires) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class Ev public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; end Ev; "
        "class Counter "
        "  public async event Ticked: Ev; "
        "  public total: int := 0; "
        "  public async method Fire(es: AsyncEventSource<Ev>, n: int): Task "
        "    await es.RaiseAsync(new Ev(n)); end Fire; "
        "  public async method Go(): Task<int> "
        "    using var s := Ticked.Subscribe(async lambda(e) => do total := total + e.N; end); "
        "    await Fire(Ticked, 7); return total; "
        "  end Go; "
        "end Counter; "
        "class C public static async method Main(): Task<int> "
        "  using var c := new Counter(); return await c.Go(); end Main; end C;"), 7);
}

// CONTROL: a direct `Ticked.Raise(...)` (no intervening local/parameter) must
// keep working — the fix must not regress the EventSymbol-keyed path.
BEZEL_TEST(codegen_event_source_var_1610, direct_raise_unregressed) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class Ev public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; end Ev; "
        "class Counter "
        "  public event Ticked: Ev; "
        "  public total: int := 0; "
        "  public method FireDirect(n: int) Ticked.Raise(new Ev(n)); end FireDirect; "
        "  public method Go(): int "
        "    using var s := Ticked.Subscribe(lambda(e) => do total := total + e.N; end); "
        "    FireDirect(3); FireDirect(4); return total; "
        "  end Go; "
        "end Counter; "
        "class C public static method Main(): int "
        "  using var c := new Counter(); return c.Go(); end Main; end C;"), 7);
}
