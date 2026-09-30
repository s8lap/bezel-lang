// fix #4968 (stdlib §6.5; spec §27.1 / §27.1.1 / §27.6 / §28.14, v0.307): a
// plain `event` member / `IEvent<T>` face / `EventSource<T>` accepted ANY
// function value as the handler argument of a DIRECT `Subscribe` / `Add` /
// `Remove` call — only the `+=` / `-=` desugaring (#4869) was checked. Stdlib
// §6.5 declares
//   public method Subscribe(scoped handler: (T) -> ()): Subscription;
//   public method Add(scoped handler: (T) -> ());
//   public method Remove(handler: (T) -> ());
// (AsyncEventSource / IAsyncEvent: `(T) -> Task`), and §27.1 / §27.1.1 make
// the modifier slots part of the function type, arity always significant, and
// primitive parameter slots invariant. Pre-fix the issue's repros compiled
// and codegen invoked the handler against the raw payload:
//   c06 overloaded group (string-first)  -> SEGV in H$On
//   c08 arity-2 handler                  -> printed garbage `5/<junk>`
//   c09 `owned` formal                   -> heap-use-after-free in H$OnB
//
// §27.6 permits a method reference to bridge a §5.1 conversion through an
// adapter and resolves an overloaded group against the target type; the
// compiler implements neither at an event-handler position (#4978), so those
// shapes are rejected here — a deliberate, SOUND spurious reject (a silent
// wrong-code run is worse). Class / interface contravariance (§27.1.1)
// needs no adapter and still RUNS — the accept rows assert exact stdout.
//
// Accept rows use `Subscribe` + `Dispose` (not `Add`), so they are clean
// under the ASan leg too: an `Add` never `Remove`d leaks 48 B (#4976).

#include <string>

#include "support/aot_support.hpp"  // runAot — AOT + native exec oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool hasDiag(const bezel::test::JitResult& r, const char* a,
             const char* b = nullptr) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(a) != std::string::npos &&
            (!b || d.message.find(b) != std::string::npos))
            return true;
    return false;
}

// `H` holds the handler set; `Btn` exposes the three plain routes: an
// `event` member, an `IEvent<int>` face over an EventSource, and the
// EventSource itself (`Direct`). `wire` runs inside Btn.Wire().
std::string prog(const std::string& handlers, const std::string& wire,
                 const std::string& payload = "int",
                 const std::string& fireArg = "5") {
    return "module M;\n" + handlers +
           "class Btn\n"
           "  private owned readonly _src: EventSource<" + payload + "> := new();\n"
           "  public property Face: IEvent<" + payload + "> => _src;\n"
           "  public event Clicked: " + payload + ";\n"
           "  public method Wire()\n    " + wire + "\n  end Wire;\n"
           "  public method Fire(n: " + payload + ")\n"
           "    Clicked.Raise(n); _src.Raise(n);\n"
           "  end Fire;\n"
           "end Btn;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    using var b := new Btn();\n"
           "    b.Wire();\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

const char* kIntH =
    "class H\n"
    "  public static method On(x: int) Console.WriteLine($\"int:{x}\"); end On;\n"
    "end H;\n";

}  // namespace

// ---------------------------------------------------------------------------
// REJECT — the issue's three repros (event member `Subscribe`).
// ---------------------------------------------------------------------------
// #4978 flip (§27.6, v0.313): the overloaded group is now resolved against
// the `(int) -> ()` slot, so the `int` overload is bound and RUNS (it was a
// sound spurious reject while only the first-declared overload was tried).
BEZEL_TEST(plain_event_handler_signature_4968,
           c06_overloaded_group_string_first_selects_int_stdlib_6_5_spec_27_6) {
    auto r = runAot(prog(
        "class H\n"
        "  public static method On(x: string) Console.WriteLine($\"str:{x}\"); end On;\n"
        "  public static method On(x: int) Console.WriteLine($\"int:{x}\"); end On;\n"
        "end H;\n",
        "using var s := Clicked.Subscribe(H.On); Fire(5);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("int:5\n"));
}

BEZEL_TEST(plain_event_handler_signature_4968,
           c08_arity_two_handler_rejected_stdlib_6_5_spec_27_1_1) {
    auto r = runAot(prog(
        "class H\n"
        "  public static method On(x: int, y: int) Console.WriteLine($\"{x}/{y}\"); end On;\n"
        "end H;\n",
        "using var s := Clicked.Subscribe(H.On); Fire(5);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "handler argument of `Subscribe` (stdlib §6.5",
                              "not implicitly convertible"));
}

BEZEL_TEST(plain_event_handler_signature_4968,
           c09_owned_formal_rejected_stdlib_6_5_spec_27_1) {
    const std::string handlers =
        "class Msg\n"
        "  public N: int := 0;\n"
        "  public constructor(n: int) N := n; end;\n"
        "end Msg;\n"
        "class H\n"
        "  public static method OnB(owned m: Msg) Console.WriteLine($\"B:{m.N}\"); end OnB;\n"
        "end H;\n"
        "class Btn\n"
        "  public event Clicked: Msg;\n"
        "  public method Fire(n: int) Clicked.Raise(new Msg(n)); end Fire;\n"
        "end Btn;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var b := new Btn();\n"
        "    var s2 := b.Clicked.Subscribe(H.OnB);\n"
        "    b.Fire(7);\n"
        "    s2.Dispose();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot("module M;\n" + handlers);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "handler argument of `Subscribe` (stdlib §6.5",
                              "parameter 1 is `owned`"));
}

// ---------------------------------------------------------------------------
// REJECT — the other plain routes (Add, IEvent face, EventSource, Remove).
// ---------------------------------------------------------------------------
BEZEL_TEST(plain_event_handler_signature_4968,
           event_member_add_string_handler_rejected_stdlib_6_5) {
    auto r = runAot(prog(
        "class H\n"
        "  public static method On(x: string) Console.WriteLine(x); end On;\n"
        "end H;\n",
        "Clicked.Add(H.On);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "handler argument of `Add` (stdlib §6.5",
                              "not implicitly convertible"));
}

BEZEL_TEST(plain_event_handler_signature_4968,
           ievent_face_subscribe_arity_rejected_stdlib_6_5) {
    auto r = runAot(prog(
        "class H\n"
        "  public static method On(x: int, y: int) Console.WriteLine($\"{x}/{y}\"); end On;\n"
        "end H;\n",
        "using var s := Face.Subscribe(H.On); Fire(5);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "handler argument of `Subscribe` (stdlib §6.5",
                              "not implicitly convertible"));
}

BEZEL_TEST(plain_event_handler_signature_4968,
           eventsource_remove_zero_arity_rejected_stdlib_6_5) {
    auto r = runAot(prog(
        "class H\n"
        "  public static method On0() Console.WriteLine(\"zero\"); end On0;\n"
        "end H;\n",
        "_src.Remove(H.On0);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "handler argument of `Remove` (stdlib §6.5",
                              "not implicitly convertible"));
}

BEZEL_TEST(plain_event_handler_signature_4968,
           eventsource_subscribe_lambda_arity_two_rejected_stdlib_6_5) {
    auto r = runAot(prog(kIntH,
        "using var s := _src.Subscribe(lambda(a, c) => Console.WriteLine($\"{a}{c}\")); Fire(1);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "event handler lambda has 2 parameters",
                              "`Subscribe` on an event source"));
}

BEZEL_TEST(plain_event_handler_signature_4968,
           eventsource_subscribe_lambda_string_annotation_rejected_spec_27_1_1) {
    auto r = runAot(prog(kIntH,
        "using var s := _src.Subscribe(lambda(t: string) => Console.WriteLine(t)); Fire(1);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "annotated `string`",
                              "does not accept the event payload type `int`"));
}

// §6.5: the sync handler type is `(T) -> ()` — a value-returning method is
// not that type (§27.1: the return is part of the function type).
BEZEL_TEST(plain_event_handler_signature_4968,
           eventsource_subscribe_value_returning_handler_rejected_stdlib_6_5) {
    auto r = runAot(prog(
        "class H\n"
        "  public static method OnI(x: int): int Console.WriteLine($\"ret:{x}\"); return x; end OnI;\n"
        "end H;\n",
        "using var s := _src.Subscribe(H.OnI); Fire(2);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "handler argument of `Subscribe` (stdlib §6.5",
                              "returns `int`"));
}

// A payload-less `event Ping` takes a `() -> ()` handler.
BEZEL_TEST(plain_event_handler_signature_4968,
           unit_event_subscribe_one_param_handler_rejected_stdlib_6_5) {
    auto r = runAot(
        "module M;\n"
        "class H\n"
        "  public static method On(x: int) Console.WriteLine($\"int:{x}\"); end On;\n"
        "end H;\n"
        "class Btn\n"
        "  public event Ping;\n"
        "  public method Fire() Ping.Raise(); end Fire;\n"
        "end Btn;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var b := new Btn();\n"
        "    var sub := b.Ping.Subscribe(H.On);\n"
        "    b.Fire();\n"
        "    sub.Dispose();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "handler argument of `Subscribe` (stdlib §6.5",
                              "not implicitly convertible"));
}

// AsyncEventSource<T>.Add takes `(T) -> Task`; a unit-returning handler
// SEGV'd in RaiseAsync pre-fix.
BEZEL_TEST(plain_event_handler_signature_4968,
           async_eventsource_add_unit_handler_rejected_stdlib_6_5) {
    auto r = runAot(
        "module M;\n"
        "class H\n"
        "  public static method OnU(x: int) Console.WriteLine($\"u:{x}\"); end OnU;\n"
        "end H;\n"
        "class Btn\n"
        "  private owned readonly _as: AsyncEventSource<int> := new();\n"
        "  public method Wire() _as.Add(H.OnU); end Wire;\n"
        "  public async method FireA(n: int): Task await _as.RaiseAsync(n); end FireA;\n"
        "end Btn;\n"
        "class C\n"
        "  public static async method Main(): Task<int>\n"
        "    using var b := new Btn();\n"
        "    b.Wire();\n"
        "    await b.FireA(6);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "handler argument of `Add` (stdlib §6.5",
                              "returns `()`"));
}

// §27.6 adapter-bridged formation (int -> double) is not implemented at an
// event-handler position (#4978): a sound spurious reject, not garbage.
BEZEL_TEST(plain_event_handler_signature_4968,
           conversion_bridged_handler_rejected_until_4978_spec_27_6) {
    auto r = runAot(prog(
        "class H\n"
        "  public static method On(x: double) Console.WriteLine($\"d:{x}\"); end On;\n"
        "end H;\n",
        "using var s := _src.Subscribe(H.On); Fire(5);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "takes `double`",
                              "only through a representation-changing conversion"));
}

// ---------------------------------------------------------------------------
// ACCEPT — well-typed handlers on every route RUN and print the payload.
// ---------------------------------------------------------------------------
BEZEL_TEST(plain_event_handler_signature_4968,
           well_typed_handler_runs_on_all_three_routes_stdlib_6_5) {
    auto r = runAot(prog(kIntH,
        "using var a := Clicked.Subscribe(H.On);\n"
        "    using var f := Face.Subscribe(H.On);\n"
        "    using var d := _src.Subscribe(lambda(n) => Console.WriteLine($\"lam:{n}\"));\n"
        "    Fire(5);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("int:5\nint:5\nlam:5\n"));
}

// An overloaded group whose FIRST overload fits binds that overload.
BEZEL_TEST(plain_event_handler_signature_4968,
           overloaded_group_int_first_runs_stdlib_6_5) {
    auto r = runAot(prog(
        "class H\n"
        "  public static method On(x: int) Console.WriteLine($\"int:{x}\"); end On;\n"
        "  public static method On(x: string) Console.WriteLine($\"str:{x}\"); end On;\n"
        "end H;\n",
        "using var s := _src.Subscribe(H.On); Fire(5);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("int:5\n"));
}

// §27.1.1: a class-typed parameter slot is contravariant — an `(Animal) -> ()`
// handler is usable where `(Dog) -> ()` is expected, with no adapter.
BEZEL_TEST(plain_event_handler_signature_4968,
           class_contravariant_handler_runs_spec_27_1_1) {
    auto r = runAot(
        "module M;\n"
        "class Animal public N: int := 3; end Animal;\n"
        "class Dog: Animal public constructor() N := 7; end; end Dog;\n"
        "class H\n"
        "  public static method On(a: Animal) Console.WriteLine($\"v:{a.N}\"); end On;\n"
        "end H;\n"
        "class Btn\n"
        "  private owned readonly _src: EventSource<Dog> := new();\n"
        "  public method Run()\n"
        "    using var s := _src.Subscribe(H.On);\n"
        "    using var d := new Dog();\n"
        "    _src.Raise(d);\n"
        "  end Run;\n"
        "end Btn;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var b := new Btn();\n"
        "    b.Run();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("v:7\n"));
}

// The retired payload-less `event Ping;` form (§24, #1727) keeps admitting
// both handler shapes the suite has always run: a zero-parameter method
// reference and the `(()) -> ()` shape stdlib §6.5 gives EventSource<()>.
BEZEL_TEST(plain_event_handler_signature_4968,
           payloadless_event_zero_and_unit_param_handlers_run_stdlib_6_5) {
    auto r = runAot(
        "module M;\n"
        "class H\n"
        "  public static method On0() Console.WriteLine(\"zero\"); end On0;\n"
        "end H;\n"
        "class Btn\n"
        "  public event Ping;\n"
        "  public method Fire() Ping.Raise(); end Fire;\n"
        "end Btn;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var b := new Btn();\n"
        "    using var s0 := b.Ping.Subscribe(H.On0);\n"
        "    using var s1 := b.Ping.Subscribe(lambda(_) => Console.WriteLine(\"unit\"));\n"
        "    b.Fire();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("zero\nunit\n"));
}
