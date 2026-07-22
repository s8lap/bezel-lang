// Codegen — user construction of a producer-side event source
// (`new EventSource<T>()` / `new AsyncEventSource<T>()`) and its disposal as an
// owned field (stdlib §6.5, v0.187; language spec §24.3; issue #1646).
//
// stdlib §6.5 (v0.187): "The constructors are public as of v0.187 ... the
// pattern constructs its own sources." The canonical event pattern is one
// `owned readonly` field holding the source, initialized with the §17.7 `new()`
// shorthand:
//
//     private owned readonly _clicked: EventSource<ClickedEvent> := new();
//     public property Clicked: IEvent<ClickedEvent> => _clicked;
//
// Before #1646 the compiler ADMITTED user construction (sema types it) but
// MISCOMPILED it in two shapes:
//   * Shape 1 — ctor-body assignment (`_e := new EventSource<T>();`): silently
//     no-op'd. lowerNew returned null for the EventSource construction, so the
//     `owned readonly` field was left null (calloc-zeroed); a later Subscribe/
//     Raise then dispatched against a null bz_event_source_t* — handlers never
//     fired (Total stays 0), program ran to exit 0 with no observable effect.
//   * Shape 2 — field-initializer position (`_e: EventSource<T> := new();` and
//     the explicit `:= new EventSource<T>()` spelling): a codegen ICE
//     ("internal: lowerExpr returned null for instance field/property
//     initializer — #1386"), because emitInstanceInit's generic lowerExpr path
//     required a non-null value.
//
// The fix, in three parts:
//   1. lowerNew recognizes isEventSourceTypeName(rt) and allocates a fresh,
//      empty source via bz_event_source_new — the same runtime object
//      emitInstanceInit mints for a keyword-form event and that
//      Subscribe/Add/Remove/Raise/RaiseAsync dispatch against
//      (tryLowerEventCall). This fixes BOTH the ICE (the field-init path now
//      gets a value) and the no-op (the ctor-assigned field now holds a real
//      source). Sync and async sources share one runtime representation.
//   2. emitOwnedFieldDisposals classifies an owned EventSource<T>/
//      AsyncEventSource<T> field (isOwnedEventSourceField) and frees it via
//      bz_event_source_free (null-safe) — reclaiming the subscriber list and
//      the struct exactly once. Without this the source (and its handler array)
//      leaks with the disposed owner.
//   3. the ctor-body exception-cleanup scan (hasOwnedToClean) includes such a
//      field so a throw after assignment frees it on the unwind path.
//
// Spec authority:
//   - stdlib §6.5 (v0.187): `EventSource<T>` / `AsyncEventSource<T>` have PUBLIC
//     constructors; the `owned readonly` field pattern; Subscribe/Raise/
//     RaiseAsync semantics; "the source's destructor severs the subscriber list
//     before reclamation" (quiescence).
//   - §24.3: inside the declaring class the source's members (Subscribe/Raise/
//     RaiseAsync) are available.
//   - §28.5 / §28.4 step 2: every owned field is disposed exactly once when its
//     owner is disposed.
//
// Oracle: runAotChecked compiles with generated-code AddressSanitizer and runs
// under LeakSanitizer, so a leak / double-free / use-after-free of the owned
// source flips r.ran to false (-> -999). Each program subscribes, raises, and
// observes the handler firing through a static accumulator returned as the exit
// code — so the SINGLE checked run pins all three failure modes: the ICE
// (compile fails), the silent no-op (wrong accumulator), and the leak (ASan
// fires). Each case FAILS without the fix and PASSES with it.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Compile with generated-code ASan, run under LSan, return the exit code — the
// construct+subscribe+raise+observe AND dispose-exactly-once oracle in one run.
// A leak / double-free / use-after-free FAILS the run (ran=false -> -999).
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// A static accumulator (Total._sum) that subscribed handlers add to, so handler
// firing is observable through the program's exit code. Shared preamble.
const char* kPreamble =
    "module M;\n"
    "import System.Threading;\n"
    "record TickEvent(N: int);\n"
    "class Total\n"
    "  public static _sum: int := 0;\n"
    "  public static method Add(n: int) _sum := _sum + n; end Add;\n"
    "end Total;\n";

// Shape 2 (field-initializer, `:= new()` §17.7 shorthand) — was the #1386 ICE.
// Subscribe INSIDE the class (producer face) and OUTSIDE via the IEvent<T>
// property; raise twice; observe both handlers fire. sum = 3 + (4 + 40) = 47.
const std::string kFieldInitShorthand =
    std::string(kPreamble) +
    "class Clock\n"
    "  private owned readonly _ticked: EventSource<TickEvent> := new();\n"
    "  public property Ticked: IEvent<TickEvent> => _ticked;\n"
    "  public method SubscribeInside(): owned Subscription\n"
    "    return _ticked.Subscribe(lambda(e) => do Total.Add(e.N); end);\n"
    "  end SubscribeInside;\n"
    "  public method Tick(n: int) _ticked.Raise(new TickEvent(n)); end Tick;\n"
    "end Clock;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    do\n"
    "      using var c := new Clock();\n"
    "      using var s1 := c.SubscribeInside();\n"
    "      c.Tick(3);\n"
    "      using var s2 := c.Ticked.Subscribe(lambda(e) => do Total.Add(e.N * 10); end);\n"
    "      c.Tick(4);\n"
    "    end;\n"
    "    return Total._sum;\n"
    "  end Main;\n"
    "end C;\n";

// Shape 2 variant — the explicit `:= new EventSource<T>()` field-init spelling
// (also the #1386 ICE before the fix). Same observable (47).
const std::string kFieldInitExplicit =
    std::string(kPreamble) +
    "class Clock\n"
    "  private owned readonly _ticked: EventSource<TickEvent>"
    " := new EventSource<TickEvent>();\n"
    "  public property Ticked: IEvent<TickEvent> => _ticked;\n"
    "  public method SubscribeInside(): owned Subscription\n"
    "    return _ticked.Subscribe(lambda(e) => do Total.Add(e.N); end);\n"
    "  end SubscribeInside;\n"
    "  public method Tick(n: int) _ticked.Raise(new TickEvent(n)); end Tick;\n"
    "end Clock;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    do\n"
    "      using var c := new Clock();\n"
    "      using var s1 := c.SubscribeInside();\n"
    "      c.Tick(3);\n"
    "      using var s2 := c.Ticked.Subscribe(lambda(e) => do Total.Add(e.N * 10); end);\n"
    "      c.Tick(4);\n"
    "    end;\n"
    "    return Total._sum;\n"
    "  end Main;\n"
    "end C;\n";

// Shape 1 (constructor-body assignment) — was the silent no-op. The field is
// declared without an initializer and assigned `new EventSource<T>()` in the
// ctor body. Same observable (47).
const std::string kCtorBodyAssign =
    std::string(kPreamble) +
    "class Clock\n"
    "  private owned readonly _ticked: EventSource<TickEvent>;\n"
    "  public constructor() _ticked := new EventSource<TickEvent>(); end constructor;\n"
    "  public property Ticked: IEvent<TickEvent> => _ticked;\n"
    "  public method SubscribeInside(): owned Subscription\n"
    "    return _ticked.Subscribe(lambda(e) => do Total.Add(e.N); end);\n"
    "  end SubscribeInside;\n"
    "  public method Tick(n: int) _ticked.Raise(new TickEvent(n)); end Tick;\n"
    "end Clock;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    do\n"
    "      using var c := new Clock();\n"
    "      using var s1 := c.SubscribeInside();\n"
    "      c.Tick(3);\n"
    "      using var s2 := c.Ticked.Subscribe(lambda(e) => do Total.Add(e.N * 10); end);\n"
    "      c.Tick(4);\n"
    "    end;\n"
    "    return Total._sum;\n"
    "  end Main;\n"
    "end C;\n";

// AsyncEventSource<T> shares the same construction/free runtime path. Construct
// via the `:= new()` shorthand, subscribe an async handler, RaiseAsync, observe
// the handler firing. sum = 5.
const std::string kAsyncFieldInit =
    std::string(kPreamble) +
    "class AsyncClock\n"
    "  private owned readonly _ticked: AsyncEventSource<TickEvent> := new();\n"
    "  public property Ticked: IAsyncEvent<TickEvent> => _ticked;\n"
    "  public method SubscribeInside(): owned Subscription\n"
    "    return _ticked.Subscribe(async lambda(e) => do Total.Add(e.N); end);\n"
    "  end SubscribeInside;\n"
    "  public async method TickAsync(n: int): Task\n"
    "    await _ticked.RaiseAsync(new TickEvent(n));\n"
    "  end TickAsync;\n"
    "end AsyncClock;\n"
    "class C\n"
    "  public static async method Run(): Task\n"
    "    using var c := new AsyncClock();\n"
    "    using var s1 := c.SubscribeInside();\n"
    "    await c.TickAsync(5);\n"
    "  end Run;\n"
    "  public static method Main(): int\n"
    "    Run().Wait();\n"
    "    return Total._sum;\n"
    "  end Main;\n"
    "end C;\n";

}  // namespace

// stdlib §6.5 (v0.187) / §24.3 — field-initializer `:= new()` shorthand: the
// user-constructed source is functional (handlers fire) and disposed leak-clean.
// Before #1646: codegen ICE (#1386).
BEZEL_TEST(codegen_event_source_user_construct_1646,
           field_init_new_shorthand_constructs_subscribes_raises_leak_free_spec_stdlib_6_5_v0_187) {
    BEZEL_EXPECT_EQ(run(kFieldInitShorthand), 47);
}

// stdlib §6.5 (v0.187) — field-initializer, explicit `:= new EventSource<T>()`
// spelling. Before #1646: codegen ICE (#1386).
BEZEL_TEST(codegen_event_source_user_construct_1646,
           field_init_explicit_type_constructs_subscribes_raises_leak_free_spec_stdlib_6_5_v0_187) {
    BEZEL_EXPECT_EQ(run(kFieldInitExplicit), 47);
}

// stdlib §6.5 (v0.187) — constructor-body assignment. Before #1646: silent
// no-op (source left null, handlers never fired, Total stayed 0).
BEZEL_TEST(codegen_event_source_user_construct_1646,
           ctor_body_assignment_constructs_subscribes_raises_leak_free_spec_stdlib_6_5_v0_187) {
    BEZEL_EXPECT_EQ(run(kCtorBodyAssign), 47);
}

// stdlib §6.5 (v0.187) — AsyncEventSource<T> shares the construct/free path;
// RaiseAsync reaches the async handler. Before #1646: ICE (field-init).
BEZEL_TEST(codegen_event_source_user_construct_1646,
           async_event_source_field_init_raise_async_leak_free_spec_stdlib_6_5_v0_187) {
    BEZEL_EXPECT_EQ(run(kAsyncFieldInit), 5);
}
