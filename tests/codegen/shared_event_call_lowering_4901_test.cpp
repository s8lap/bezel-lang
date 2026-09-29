// fix #4901 (stdlib §6.5 "Events: IEvent, EventSource, ISharedEvent,
// SharedEventSource", v0.307): codegen's `Emitter::tryLowerEventCall`
// (LlvmBackend_Calls2.cpp) recognized `IEvent<T>`/`EventSource<T>` receivers
// but had NO arms for the shared (cross-domain) pair `ISharedEvent<T>` /
// `SharedEventSource<T>` at all — a call resolving to `.Subscribe` through an
// `ISharedEvent<T>`-typed receiver, or to `.Subscribe`/`.Add`/`.Remove`/
// `.Raise` through a `SharedEventSource<T>`-typed receiver, fell through to
// ordinary (itable) call lowering and either produced no value (compile-time
// backstop #2457: "produced no value") or, pre-#2457, silently compiled to a
// no-op that printed nothing. Separate from #4857 (a different receiver
// shape on the same dispatcher).
//
// THE FIX adds two arms to `tryLowerEventCall`, mirroring the existing
// `IEvent<T>`/`EventSource<T>` arms exactly:
//   - `ISharedEvent<T>.Subscribe` (the interface face; stdlib §6.5 declares
//     ONLY `Subscribe` on this interface — no Add/Remove/Raise, and it does
//     NOT extend `IEvent<T>` by design, so it is unreached by the existing
//     `isIEventTypeName` arm).
//   - `SharedEventSource<T>.{Subscribe,Add,Remove,Raise}` (the sealed
//     producer class). `RaiseAsync` is deliberately EXCLUDED: stdlib §6.5
//     does not declare it on `SharedEventSource<T>` (the shared async twin
//     is deferred), and unlike `EventSource<T>`/`AsyncEventSource<T>` (gated
//     by a sema `rcn == "EventSource"` member-surface check in
//     ExprTypecheck_Calls.cpp that rejects any undeclared member name before
//     codegen runs), there is currently NO equivalent sema gate for
//     `SharedEventSource<T>` — so this arm names the four real methods
//     explicitly rather than forwarding every name, leaving an
//     out-of-surface call (e.g. a bogus `.RaiseAsync()`) to fall through to
//     ordinary dispatch instead of being mis-lowered as an async raise.
//
// A THIRD, closely related fix rides along in the same commit:
// `Emitter::isSubscriptionExpr`'s structural `isSubscribeCall` fallback (the
// mechanism that recognizes a `.Subscribe(...)` result as an owned
// `Subscription` needing disposal) did not recognize either of the two new
// receiver type names. Sema has no `Subscription`-typing block for this pair
// either (the same gap noted above), so that fallback is the ONLY place
// disposal recognizes these calls' results; without this arm, disposing a
// `Subscription` obtained through an `ISharedEvent<T>`/`SharedEventSource<T>`
// receiver dispatched through ordinary vtable lookup against a vtable-less
// handle — a SEGV, measured against this fix's own probe program pre-fix.
//
// Capture-freedom (stdlib §6.5, "Capture-freedom at every subscribe
// position"): every subscribe position on the shared pair admits ONLY a
// non-capturing lambda or a static method reference. Sema has NO enforcement
// of this rule for ISharedEvent<T>/SharedEventSource<T> today (tracked
// separately as issue #4920, filed alongside this fix) — the only two
// existing capture-freedom checks in the compiler are scoped to the
// unrelated `static event` member-kind keyword syntax and to Domain.Spawn.
// Recognizing `Subscribe`/`Add` unconditionally would therefore have made
// this fix SILENTLY ACCEPT a capturing lambda the spec requires rejected
// (measured: doing so flips conformance corpus rows l6_5_0004 and
// s22_12_0017 from baseline `pass` to compiled-when-the-spec-rejects). So
// `tryLowerEventCall`'s `Subscribe`/`Add` arms for the shared pair claim the
// call ONLY when the handler RESOLVES to a STATIC method reference (the
// resolved MethodSymbol carries `static`; never keyed on the AST spelling).
// An INSTANCE method reference is NOT capture-free — spec §28.14 (v0.307): a
// method reference's "capture is the receiver" — and a local / conditional /
// call-result handler may hold a capturing closure; all of those, and every
// lambda literal, stay on the pre-existing #2457 backstop until #4920 lands
// the real sema check. (The first cut of this fix gated NEGATIVELY — "not a
// lambda literal" — and so admitted capturing locals and instance method
// groups, several of which were a heap-use-after-free under ASan; the
// negative rows at the bottom of this file pin that each such form is
// REJECTED, asserting the specific #2457 diagnostic text.) Every positive
// handler below is a static method reference — `Bus.OnA` or bare `OnX` —
// matching the stdlib's own `TickBus.Forward`/`App.Exiting.Subscribe(OnExit)`
// idiom (§6.5).
//
// v3 (review of v2): capture-freedom is not enough, because sema has no
// member surface for the pair and so checks NOTHING about these calls. The
// gate (sharedEventHandlerAdmissible) therefore also requires the static
// handler to have exactly the handler type `(T) -> ()` — not an overload
// group, one formal with no modifier whose type is T, unit return — and it
// now also guards the `+=` desugaring (tryLowerEventCompound, which admitted
// any handler), `Remove` (same gate: a lambda-literal argument can never
// compare equal to a subscribed handler, and a capturing one leaked), and
// `Raise` (sharedEventRaiseAdmissible: one argument of type exactly T, or
// none for a unit payload). The v3 rows at the bottom pin each.
//
// #4920 (cherry-picked beneath this commit) now enforces the CAPTURE half in
// sema at Subscribe / Add / `+=`, so the capturing-handler rows assert its
// §6.5 diagnostic instead of #2457. It does not check the handler's signature,
// Remove, or Raise, and it admits capture-free lambdas that codegen does not
// yet lower (#4972), so the codegen gate stays and those rows still assert
// the codegen diagnostics.
// Every lambda parameter in this file is explicitly typed to avoid depending
// on issue #4792's unfinished lambda parameter type inference.

#include <initializer_list>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

// Plain oracle: exact exit code, or -999 if the program did not run
// (compile/link/runtime failure) — the FAIL-WITHOUT signal for every row that
// pre-fix failed to compile (backstop #2457) rather than mis-executing.
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// LEAK/CRASH oracle: runAotChecked instruments the GENERATED program with
// ASan/LSan and treats ANY sanitizer finding (including LSan's exit-time leak
// report) as ran=false. Used for the loop-amplified row only.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// REJECT oracle (per tests/README.md: runAot also synthesizes an Error
// diagnostic for a runtime crash, so a compile-time rejection is proved by
// the diagnostic TEXT, never by `!ran` alone). True iff the program did not
// run AND one Error diagnostic carries EVERY keyword.
bool rejectedWith(const std::string& program,
                  std::initializer_list<const char*> keywords) {
    auto r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        bool all = true;
        for (const char* k : keywords) {
            if (d.message.find(k) == std::string::npos) { all = false; break; }
        }
        if (all) return true;
    }
    return false;
}

// Shared program prefix for the negative rows: a SharedEventSource<int>
// field, its ISharedEvent<int> face, and a Box whose INSTANCE method OnI
// mutates (and prints) receiver state.
const char* kNegPrelude =
    "module M; "
    "import System.Threading; "
    "class Bus "
    "  public static owned readonly Feed: SharedEventSource<int> := new(); "
    "  public static property Face: ISharedEvent<int> => Feed; "
    "  public static method OnX(x: int) Console.WriteLine(x); end OnX; "
    "end Bus; "
    "class Box "
    "  public V: int := 0; "
    "  public method Add(x: int) V := V + x; end Add; "
    "  public method OnI(x: int) V := V + x; Console.WriteLine(V); end OnI; ";

}  // namespace

// -----------------------------------------------------------------------
// SharedEventSource<T> as a DIRECT receiver (concrete type, not through the
// ISharedEvent<T> face) — exercises the new SharedEventSource<T> arm's
// Subscribe AND Raise branches on the same receiver.
// -----------------------------------------------------------------------
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           subscribe_and_raise_directly_on_shared_event_source_receiver) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System.Threading; "
        "class Bus "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "  public static Total: int := 0; "
        "  public static method OnX(x: int) Total := Total + x; end OnX; "
        "end Bus; "
        "class C "
        "  public static method Main(): int "
        "    var sub := Bus.Feed.Subscribe(Bus.OnX); "
        "    Bus.Feed.Raise(9); "
        "    sub.Dispose(); "
        "    return Bus.Total; "
        "  end Main; "
        "end C;"),
        9);
}

// -----------------------------------------------------------------------
// The canonical stdlib §6.5 idiom: Subscribe through the ISharedEvent<T>
// interface FACE (a static expression-bodied property); Raise from inside
// the declaring class through the backing SharedEventSource<T> field.
// Exercises the new ISharedEvent<T> arm.
// -----------------------------------------------------------------------
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           subscribe_via_ishared_event_interface_face_then_raise_via_source) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System.Threading; "
        "class Bus "
        "  private static owned readonly _feed: SharedEventSource<int> := new(); "
        "  public static property Feed: ISharedEvent<int> => _feed; "
        "  public static Total: int := 0; "
        "  public static method Publish(x: int) _feed.Raise(x); end Publish; "
        "  public static method OnX(x: int) Total := Total + x; end OnX; "
        "end Bus; "
        "class C "
        "  public static method Main(): int "
        "    var sub := Bus.Feed.Subscribe(Bus.OnX); "
        "    Bus.Publish(5); "
        "    Bus.Publish(7); "
        "    sub.Dispose(); "
        "    return Bus.Total; "                              // expect 5 + 7
        "  end Main; "
        "end C;"),
        12);
}

// -----------------------------------------------------------------------
// Multiple handlers fire in SUBSCRIPTION order on a single Raise (stdlib
// §6.5 "Raising": "invoke handlers in subscription order, over a snapshot").
// Handlers are bare static method references (§6.5's `TickBus.Forward`
// idiom), each printing a tagged line — proves ORDER, which an accumulated
// sum (commutative) would not.
// -----------------------------------------------------------------------
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           multiple_handlers_fire_in_subscription_order) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "import System.Threading; "
        "class Bus "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "  public static method OnA(x: int) Console.WriteLine($\"A:{x}\"); end OnA; "
        "  public static method OnB(x: int) Console.WriteLine($\"B:{x}\"); end OnB; "
        "end Bus; "
        "class C "
        "  public static method Main(): int "
        "    var s1 := Bus.Feed.Subscribe(Bus.OnA); "
        "    var s2 := Bus.Feed.Subscribe(Bus.OnB); "
        "    Bus.Feed.Raise(1); "
        "    s1.Dispose(); "
        "    s2.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "A:1\nB:1");
}

// -----------------------------------------------------------------------
// `.Add` / `.Remove` on SharedEventSource<T> (stdlib §6.5 declares both,
// call-bounded `Remove` — "compares only"). Uses the plain (non-leak-
// checking) `run()` oracle deliberately: `bz_event_remove_closure`
// (bezel_runtime_events.cpp) has a PRE-EXISTING, unrelated leak (deactivates
// a handler without releasing its stored closure reference) — confirmed via
// a control program using plain EventSource<T>.Remove, reproducing the same
// leak on origin/main, unrelated to and predating this fix. This row is a
// FUNCTIONAL assertion only; runAot's detect_leaks=0 default means the
// pre-existing leak does not affect this row's pass/fail on either the plain
// or `--full` (ASan) gate leg.
// -----------------------------------------------------------------------
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           add_then_remove_unsubscribes_one_of_two_handlers) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System.Threading; "
        "class Bus "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "  public static Total: int := 0; "
        "  public static method OnA(x: int) Total := Total + x; end OnA; "
        "  public static method OnB(x: int) Total := Total + x * 10; end OnB; "
        "end Bus; "
        "class C "
        "  public static method Main(): int "
        "    Bus.Feed.Add(Bus.OnA); "
        "    Bus.Feed.Add(Bus.OnB); "
        "    Bus.Feed.Raise(1); "                             // Total = 1 + 10 = 11
        "    Bus.Feed.Remove(Bus.OnA); "
        "    Bus.Feed.Raise(1); "                             // Total += 10 -> 21 (OnA gone)
        "    return Bus.Total; "
        "  end Main; "
        "end C;"),
        21);
}

// -----------------------------------------------------------------------
// Loop-amplified (100x) Subscribe + Raise + Dispose through a
// SharedEventSource<T> receiver with a static-method-reference handler,
// under `runAotChecked` (ASan/LSan). Proves the new codegen path does not
// leak the per-subscribe closure or the returned Subscription object over
// repeated cycles (CLAUDE.md "Oracles that pass vacuously" — a one-shot leak
// repro can pass on both sides of a fix; the loop is the mitigation).
// -----------------------------------------------------------------------
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           subscribe_raise_dispose_loop_100x_is_leak_clean) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "import System.Threading; "
        "class Bus "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "  public static method Handler(x: int) end Handler; "
        "end Bus; "
        "class C "
        "  public static method Round(i: int) "
        "    var sub := Bus.Feed.Subscribe(Bus.Handler); "
        "    Bus.Feed.Raise(i); "
        "    sub.Dispose(); "
        "  end Round; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 100 do "
        "      Round(i); "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"),
        0);
}

// -----------------------------------------------------------------------
// CONTROL: the pre-existing, unmodified instance-event path
// (`EventSource<T>`/`IEvent<T>`, the `event` member-kind desugaring) must be
// unaffected by the two new SharedEventSource<T>/ISharedEvent<T> arms.
// Mirrors event_test.cpp's own shape with an explicitly typed lambda
// parameter (this file avoids relying on #4792's inference throughout).
// `b` is bound `using var` (rather than event_test.cpp's plain `var`,
// admitted only under that file's lenient `runJit` oracle): under the
// strict `runAot` oracle this file otherwise uses, §28.14's capture
// admission requires the subscribing receiver's bounding object to resolve
// to a `using var` local (or `this`, or an owned field of `this`) before a
// capture-bearing handler (`box`, here) is admitted at all.
// -----------------------------------------------------------------------
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           event_source_control_row_unaffected_by_shared_arms) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Ev(N: int); "
        "class Box "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box; "
        "class Button "
        "  public event Clicked: Ev; "
        "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "
        "end Button; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    using var b := new Button(); "
        "    var s := b.Clicked.Subscribe(lambda(e: Ev) => do box.Add(e.N); end); "
        "    b.Fire(5); "
        "    b.Fire(7); "
        "    s.Dispose(); "
        "    return box.V; "                                  // expect 5 + 7
        "  end Main; "
        "end C;"),
        12);
}

// -----------------------------------------------------------------------
// The UNQUALIFIED static method reference spelling: bare `OnX` inside a
// static method of the declaring class resolves to the same static
// MethodSymbol as `Bus.OnX` (stdlib §6.5's `App.Exiting.Subscribe(OnExit)`
// idiom), exercising the IdentifierExpr arm of sharedEventHandlerAdmissible on
// both `Subscribe` and `Add`.
// -----------------------------------------------------------------------
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           bare_unqualified_static_method_ref_subscribe_and_add) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "import System.Threading; "
        "class Bus "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "  public static method OnX(x: int) Console.WriteLine($\"X:{x}\"); end OnX; "
        "  public static method OnY(x: int) Console.WriteLine($\"Y:{x}\"); end OnY; "
        "  public static method Arm() "
        "    var s := Feed.Subscribe(OnX); "
        "    Feed.Add(OnY); "
        "    Feed.Raise(3); "
        "    s.Dispose(); "
        "  end Arm; "
        "end Bus; "
        "class C "
        "  public static method Main(): int "
        "    Bus.Arm(); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "X:3\nY:3");
}

// -----------------------------------------------------------------------
// NEGATIVE rows — stdlib §6.5 "Capture-freedom at every subscribe position"
// / spec §28.14 (v0.307). Each row is a CAPTURING handler form the first
// (negatively gated) cut of this fix accepted — reviewer probes
// p1/p7/p3b/p11/p10/p5, several of them an ASan heap-use-after-free once the
// receiver died. Since #4920 (landed beneath this fix) sema rejects each of
// them first, so each asserts that SPECIFIC §6.5 capture diagnostic, not
// merely "did not run". tryLowerEventCall's gate (sharedEventHandlerAdmissible)
// would still decline every one of them — defence in depth.
// -----------------------------------------------------------------------

// p1 shape: a local holding a capturing lambda, SharedEventSource.Subscribe.
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           capturing_lambda_local_handler_rejected_on_source_subscribe) {
    BEZEL_EXPECT_TRUE(rejectedWith(std::string(kNegPrelude) +
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    var h := lambda(x: int) => do box.Add(x); end; "
        "    var sub := Bus.Feed.Subscribe(h); "
        "    Bus.Feed.Raise(9); "
        "    sub.Dispose(); "
        "    return box.V; "
        "  end Main; "
        "end C;",
        {"handler subscribed to an ISharedEvent<T> or SharedEventSource<T> must "
         "be capture-free",
         "stdlib \xC2\xA7""6.5, v0.307"}));
}

// p7 shape: the same capturing local, through the ISharedEvent<T> face.
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           capturing_lambda_local_handler_rejected_on_ishared_event_face) {
    BEZEL_EXPECT_TRUE(rejectedWith(std::string(kNegPrelude) +
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    var h := lambda(x: int) => do box.Add(x); end; "
        "    var sub := Bus.Face.Subscribe(h); "
        "    Bus.Feed.Raise(4); "
        "    sub.Dispose(); "
        "    return box.V; "
        "  end Main; "
        "end C;",
        {"handler subscribed to an ISharedEvent<T> or SharedEventSource<T> must "
         "be capture-free",
         "stdlib \xC2\xA7""6.5, v0.307"}));
}

// p3b shape: an INSTANCE method group `box.OnI` passed to Add in a helper;
// `box` dies at the helper's end, then Raise (heap-use-after-free if
// admitted). §28.14: the method reference's capture is the receiver.
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           instance_method_group_handler_rejected_on_source_add) {
    BEZEL_EXPECT_TRUE(rejectedWith(std::string(kNegPrelude) +
        "end Box; "
        "class C "
        "  public static method Arm() "
        "    var box := new Box(); "
        "    Bus.Feed.Add(box.OnI); "
        "  end Arm; "
        "  public static method Main(): int "
        "    Arm(); "
        "    Bus.Feed.Raise(9); "
        "    return 0; "
        "  end Main; "
        "end C;",
        {"handler subscribed to an ISharedEvent<T> or SharedEventSource<T> must "
         "be capture-free",
         "stdlib \xC2\xA7""6.5, v0.307"}));
}

// p11 shape: an instance method group through the ISharedEvent<T> face.
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           instance_method_group_handler_rejected_on_ishared_event_face) {
    BEZEL_EXPECT_TRUE(rejectedWith(std::string(kNegPrelude) +
        "end Box; "
        "class C "
        "  public static method Go() "
        "    var b := new Box(); "
        "    var sub := Bus.Face.Subscribe(b.OnI); "
        "  end Go; "
        "  public static method Main(): int "
        "    Go(); "
        "    Bus.Feed.Raise(3); "
        "    return 0; "
        "  end Main; "
        "end C;",
        {"handler subscribed to an ISharedEvent<T> or SharedEventSource<T> must "
         "be capture-free",
         "stdlib \xC2\xA7""6.5, v0.307"}));
}

// p10 shape: `this.OnI` inside an instance method — the implicit receiver is
// `this`, which dies before the Raise.
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           this_qualified_instance_method_handler_rejected_on_source_add) {
    BEZEL_EXPECT_TRUE(rejectedWith(std::string(kNegPrelude) +
        "  public method Arm() Bus.Feed.Add(this.OnI); end Arm; "
        "end Box; "
        "class C "
        "  public static method Go() "
        "    var b := new Box(); "
        "    b.Arm(); "
        "  end Go; "
        "  public static method Main(): int "
        "    Go(); "
        "    Bus.Feed.Raise(9); "
        "    return 0; "
        "  end Main; "
        "end C;",
        {"handler subscribed to an ISharedEvent<T> or SharedEventSource<T> must "
         "be capture-free",
         "stdlib \xC2\xA7""6.5, v0.307"}));
}

// p10 shape, unqualified: bare `OnI` inside an instance method resolves to
// the INSTANCE MethodSymbol (implicit `this` receiver) — the same spelling
// as the admitted bare static `OnX` row above, which is why the gate keys
// on the resolved symbol and never on the AST shape.
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           bare_instance_method_handler_rejected_on_source_add) {
    BEZEL_EXPECT_TRUE(rejectedWith(std::string(kNegPrelude) +
        "  public method Arm() Bus.Feed.Add(OnI); end Arm; "
        "end Box; "
        "class C "
        "  public static method Go() "
        "    var b := new Box(); "
        "    b.Arm(); "
        "  end Go; "
        "  public static method Main(): int "
        "    Go(); "
        "    Bus.Feed.Raise(9); "
        "    return 0; "
        "  end Main; "
        "end C;",
        {"handler subscribed to an ISharedEvent<T> or SharedEventSource<T> must "
         "be capture-free",
         "stdlib \xC2\xA7""6.5, v0.307"}));
}

// p5 shape: a conditional whose one arm is a capturing lambda local and whose
// other is a static method reference. The conditional as a whole is not a
// static method reference, so it is not admitted.
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           conditional_handler_with_capturing_arm_rejected_on_source_subscribe) {
    BEZEL_EXPECT_TRUE(rejectedWith(std::string(kNegPrelude) +
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    var c := true; "
        "    var f := lambda(x: int) => do box.Add(x); end; "
        "    var sub := Bus.Feed.Subscribe((if c then f else Bus.OnX)); "
        "    Bus.Feed.Raise(9); "
        "    sub.Dispose(); "
        "    return box.V; "
        "  end Main; "
        "end C;",
        {"handler subscribed to an ISharedEvent<T> or SharedEventSource<T> must "
         "be capture-free",
         "stdlib \xC2\xA7""6.5, v0.307"}));
}

// -----------------------------------------------------------------------
// NEGATIVE rows, v3 — the handler is capture-free (a static method
// reference) but does NOT have the handler type `(T) -> ()`. Sema has no
// member surface for the shared pair, so nothing else checks the handler's
// signature against the payload: before this gate each of these compiled and
// the handler was invoked through the `(T) -> ()` ABI (review probes p06,
// p08, p09 — SEGV, garbage second argument, heap-use-after-free). Each was
// first pinned by the codegen gate's #2457 reject; #4898 (sema
// plainEventHandlerSlot extended to the shared pair, stdlib §6.5 / spec
// §27.1.1, v0.307) now rejects the Subscribe / Add forms earlier, so these
// rows assert its handler-slot diagnostic. The `+=` forms below still reach
// the codegen gate. The overload-group row (p06) is the deliberate SOUND
// spurious reject of #4978: §27.6 resolves `H.On` against the `(int) -> ()`
// target to `On(int)`, which the compiler does not implement yet — flip it to
// an accept-and-run row when #4978 lands.
// -----------------------------------------------------------------------

// p06: an overload GROUP `H.On` (On(string) / On(int)) on an int feed. Sema
// binds the method reference to the string overload without consulting the
// target type; admitted, the int payload was read as a string — SEGV.
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           overloaded_static_method_group_rejected_on_source_subscribe) {
    BEZEL_EXPECT_TRUE(rejectedWith(std::string(kNegPrelude) +
        "end Box; "
        "class H "
        "  public static method On(x: string) Console.WriteLine($\"str:{x}\"); end On; "
        "  public static method On(x: int) Console.WriteLine($\"int:{x}\"); end On; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var sub := Bus.Feed.Subscribe(H.On); "
        "    Bus.Feed.Raise(5); "
        "    sub.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;",
        {"the handler argument of `Subscribe` (stdlib \xC2\xA7""6.5, v0.307)",
         "not implicitly convertible to the target type (\xC2\xA7""5.1)"}));
}

// p08: a static of arity 2 on an int feed — admitted, the second parameter
// was uninitialized (printed `5/<garbage>`).
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           arity_two_static_handler_rejected_on_source_subscribe) {
    BEZEL_EXPECT_TRUE(rejectedWith(std::string(kNegPrelude) +
        "end Box; "
        "class H "
        "  public static method On(x: int, y: int) Console.WriteLine($\"{x}/{y}\"); end On; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var sub := Bus.Feed.Subscribe(H.On); "
        "    Bus.Feed.Raise(5); "
        "    sub.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;",
        {"the handler argument of `Subscribe` (stdlib \xC2\xA7""6.5, v0.307)",
         "not implicitly convertible to the target type (\xC2\xA7""5.1)"}));
}

// p09: a static whose formal is `owned m: Msg`. Each handler receives a
// BORROW of the one payload; admitted, the first handler freed it and the
// second read freed memory — ASan heap-use-after-free in `Bus$OnB`.
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           owned_formal_static_handler_rejected_on_source_subscribe) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; "
        "import System.Threading; "
        "class Msg "
        "  public N: int := 0; "
        "  public constructor(n: int) N := n; end; "
        "end Msg; "
        "class Bus "
        "  public static owned readonly Feed: SharedEventSource<Msg> := new(); "
        "  public static method OnA(owned m: Msg) Console.WriteLine($\"A:{m.N}\"); end OnA; "
        "  public static method OnB(owned m: Msg) Console.WriteLine($\"B:{m.N}\"); end OnB; "
        "end Bus; "
        "class C "
        "  public static method Main(): int "
        "    var s1 := Bus.Feed.Subscribe(Bus.OnA); "
        "    var s2 := Bus.Feed.Subscribe(Bus.OnB); "
        "    Bus.Feed.Raise(new Msg(7)); "
        "    s1.Dispose(); s2.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;",
        {"the handler argument of `Subscribe` (stdlib \xC2\xA7""6.5, v0.307)",
         "parameter 1 is `owned` but the target's is (none)"}));
}

// A single, non-overloaded static whose parameter type differs from the
// payload (`string` formal on an int feed).
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           wrong_parameter_type_static_handler_rejected_on_source_add) {
    BEZEL_EXPECT_TRUE(rejectedWith(std::string(kNegPrelude) +
        "end Box; "
        "class H "
        "  public static method OnS(s: string) Console.WriteLine(s); end OnS; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    Bus.Feed.Add(H.OnS); "
        "    Bus.Feed.Raise(5); "
        "    return 0; "
        "  end Main; "
        "end C;",
        {"the handler argument of `Add` (stdlib \xC2\xA7""6.5, v0.307)",
         "not implicitly convertible to the target type (\xC2\xA7""5.1)"}));
}

// -----------------------------------------------------------------------
// NEGATIVE rows, v3 — the `+=` desugaring of `.Add` (stdlib §6.5 names it a
// subscribe position of the pair). tryLowerEventCompound (Assign.cpp)
// admitted any handler here even on the base; only the then-rejected `Raise`
// kept such a program from running. With `Raise` recognized (and before
// #4920), both of these ran: p15 a heap-use-after-free in `Box$OnI`, p16 a
// SEGV. #4920's sema `+=` arm now rejects both first, so they assert its
// diagnostic. The codegen `+=` gate (which has no #2457 backstop behind it and
// reports its own diagnostic) is pinned by the capture-free rows after them.
// -----------------------------------------------------------------------

// p15: an INSTANCE method reference `b.OnI` whose receiver dies at the end of
// `Arm`, then Raise.
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           plus_eq_instance_method_handler_rejected_on_source) {
    BEZEL_EXPECT_TRUE(rejectedWith(std::string(kNegPrelude) +
        "end Box; "
        "class C "
        "  public static method Arm() "
        "    var b := new Box(); "
        "    Bus.Feed += b.OnI; "
        "  end Arm; "
        "  public static method Main(): int "
        "    Arm(); "
        "    Bus.Feed.Raise(9); "
        "    return 0; "
        "  end Main; "
        "end C;",
        {"handler subscribed to a SharedEventSource<T> via `+=` must be "
         "capture-free",
         "stdlib \xC2\xA7""6.5, v0.307"}));
}

// p16: a capturing lambda literal over a local that dies at the end of `Arm`.
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           plus_eq_capturing_lambda_handler_rejected_on_source) {
    BEZEL_EXPECT_TRUE(rejectedWith(std::string(kNegPrelude) +
        "end Box; "
        "class C "
        "  public static method Arm() "
        "    var box := new Box(); "
        "    Bus.Feed += lambda(x: int) => do box.Add(x); end; "
        "  end Arm; "
        "  public static method Main(): int "
        "    Arm(); "
        "    Bus.Feed.Raise(9); "
        "    return 0; "
        "  end Main; "
        "end C;",
        {"handler subscribed to a SharedEventSource<T> via `+=` must be "
         "capture-free",
         "stdlib \xC2\xA7""6.5, v0.307"}));
}

// POSITIVE, v3: `+=` / `-=` with a static method reference of exactly
// `(int) -> ()` is admitted by the same gate, and `-=` unsubscribes it
// (the second Raise prints nothing).
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           plus_eq_static_method_ref_subscribes_and_minus_eq_removes) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "import System.Threading; "
        "class Bus "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "  public static method OnX(x: int) Console.WriteLine($\"X:{x}\"); end OnX; "
        "end Bus; "
        "class C "
        "  public static method Main(): int "
        "    Bus.Feed += Bus.OnX; "
        "    Bus.Feed.Raise(9); "
        "    Bus.Feed -= Bus.OnX; "
        "    Bus.Feed.Raise(10); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "X:9");
}

// -----------------------------------------------------------------------
// NEGATIVE rows, v3 — `Raise` / `Remove` argument checking. Sema checks
// neither against `Raise(payload: T)` / `Remove(handler: (T) -> ())` for the
// shared pair, and v2 claimed both unconditionally: `Raise("hello")` on an
// int feed printed a garbage int, `Raise()` on a Msg feed SEGV'd in the
// handler, `Raise(1, 2)` compiled, and `Remove(<capturing lambda>)` leaked
// every closure environment. Each keeps the #2457 reject.
// -----------------------------------------------------------------------
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           raise_with_wrong_payload_type_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(std::string(kNegPrelude) +
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var sub := Bus.Feed.Subscribe(Bus.OnX); "
        "    Bus.Feed.Raise(\"hello\"); "
        "    sub.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;",
        {"`Raise` on built-in type `SharedEventSource` produced no value",
         "(#2457)"}));
}

BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           raise_without_argument_on_class_payload_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; "
        "import System.Threading; "
        "class Msg "
        "  public N: int := 0; "
        "  public constructor(n: int) N := n; end; "
        "end Msg; "
        "class Bus "
        "  public static owned readonly Feed: SharedEventSource<Msg> := new(); "
        "  public static method OnX(m: Msg) Console.WriteLine($\"X:{m.N}\"); end OnX; "
        "end Bus; "
        "class C "
        "  public static method Main(): int "
        "    var sub := Bus.Feed.Subscribe(Bus.OnX); "
        "    Bus.Feed.Raise(); "
        "    sub.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;",
        {"`Raise` on built-in type `SharedEventSource` produced no value",
         "(#2457)"}));
}

BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           raise_with_two_arguments_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(std::string(kNegPrelude) +
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var sub := Bus.Feed.Subscribe(Bus.OnX); "
        "    Bus.Feed.Raise(1, 2); "
        "    sub.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;",
        {"`Raise` on built-in type `SharedEventSource` produced no value",
         "(#2457)"}));
}

BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           remove_with_capturing_lambda_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(std::string(kNegPrelude) +
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 100 do "
        "      var box := new Box(); "
        "      Bus.Feed.Remove(lambda(x: int) => do box.Add(x); end); "
        "      i := i + 1; "
        "    end; "
        "    Bus.Feed.Raise(1); "
        "    return 0; "
        "  end Main; "
        "end C;",
        {"`Remove` on built-in type `SharedEventSource` produced no value",
         "(#2457)"}));
}

// -----------------------------------------------------------------------
// NEGATIVE rows, v3 on #4920 — the codegen `+=` gate. #4920's sema `+=` arm
// admits every CAPTURE-FREE handler, and sema's `+=` assignability check
// (which does reject a wrong arity, a mismatched parameter type and an `owned`
// formal — measured) judges an overload group by the overload it binds, so
// these two reach tryLowerEventCompound.
// Before the gate, `+=` admitted any handler; each row's `Raise` was then
// the #2457 reject on the base, so the specific `+=` text below is what
// distinguishes the gate.
// -----------------------------------------------------------------------

// A capture-free lambda literal via `+=`: §6.5 admits it, but lowering a
// lambda handler on the shared pair is not implemented yet (#4972) — a
// spurious reject by design, pinned so the flip to a RUN row is visible.
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           plus_eq_capture_free_lambda_rejected_by_codegen_gate) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; "
        "import System.Threading; "
        "class Bus "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "end Bus; "
        "class C "
        "  public static method Main(): int "
        "    Bus.Feed += lambda(x: int) => Console.WriteLine(x); "
        "    Bus.Feed.Raise(9); "
        "    return 0; "
        "  end Main; "
        "end C;",
        {"`+=` on a `SharedEventSource<T>` is lowered only for a static, "
         "non-overloaded method reference whose signature is exactly "
         "`(T) -> ()`",
         "(#4972)"}));
}

// p06 via `+=`: an overload GROUP whose first member is `On(int)`. Sema's
// `+=` assignability check passes (measured: with `On(string)` declared first
// it rejects instead), but which overload codegen would bind is not what sema
// checked, so the gate rejects every overload group.
BEZEL_TEST(codegen_shared_event_call_lowering_4901,
           plus_eq_overloaded_static_group_rejected_by_codegen_gate) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; "
        "import System.Threading; "
        "class Bus "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "end Bus; "
        "class H "
        "  public static method On(x: int) Console.WriteLine($\"int:{x}\"); end On; "
        "  public static method On(x: string) Console.WriteLine($\"str:{x}\"); end On; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    Bus.Feed += H.On; "
        "    Bus.Feed.Raise(5); "
        "    return 0; "
        "  end Main; "
        "end C;",
        {"`+=` on a `SharedEventSource<T>` is lowered only for a static, "
         "non-overloaded method reference whose signature is exactly "
         "`(T) -> ()`",
         "(#4972)"}));
}
