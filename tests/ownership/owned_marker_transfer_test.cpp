// Spec-derived tests for the v0.169 call-site transfer marker (#1348), kept
// up to date as the marker spelling respelled to `M(move x)` at v0.286
// (fix #3783; §22.3 / §28.8.1).
//
// §22.3 (v0.286): "when the argument is a **named owned binding** — an owned
// local or an `owned` parameter, which the call leaves moved-from — the caller
// writes the transfer marker at the call site: `M(move x)` (an identifier-
// operand `move` expression, §28.8.1 — the marker is part of the argument
// expression, not an argument modifier). A **fresh producer** — a `new`
// expression, a call returning `owned T`, an operator result, or a `move
// obj._field` — stays unmarked ... Omitting the marker on a named owned binding
// is a compile error naming the fix ... writing it on a borrow, a fresh
// producer, or a non-`owned` parameter position is likewise an error."
//
// The marker is SURFACE-ONLY: §28.8 ownership transfer is unchanged; the pass
// already models the move. These tests pin the required/rejected SPELLING both
// ways, plus a codegen value test proving the marked transfer disposes exactly
// once (identical runtime behaviour to the pre-v0.169 bare form).
//
// per §A.4 / proposal 0090 (v0.246): `owned` is NOT an ArgModifiers
// alternative; the call-site transfer marker is `move` (A.7 ArgExpression MoveExpr,
// §22.3, §28.8.1). `ref`/`out`/`scoped` are the only valid ArgModifiers.

#include <string>
#include <string_view>

#include "support/aot_support.hpp"       // runAot — AOT oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// A `Sink` mirroring the owning-collection surface: `Add(owned item: T)` and a
// factory `MakeOwned(): owned T` (an owned-returning call — a fresh producer).
// The owned `item` obligation is discharged inside the body so the Sink itself
// surfaces no unrelated diagnostics. `Peek(x: T)` is a plain (borrow) parameter.
std::string mod(const std::string& body) {
    return std::string(
        "module M; "
        "class T end T; "
        "class Sink "
        "  private owned _slot: T := new T(); "
        "  public method Add(owned item: T) "
        "    using var hold := move item; "
        "  end Add; "
        "  public method MakeOwned(): owned T "
        "    return new T(); "
        "  end MakeOwned; "
        "  public method Peek(x: T) "
        "    _ := 1; "
        "  end Peek; "
        "end Sink; ") + body;
}

// True iff some error diagnostic's message contains `needle`.
bool errorSays(const bezel::test::OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// PARSER (A.4): `owned` is NOT an ArgModifiers alternative.
// ---------------------------------------------------------------------------

// per §A.4 / proposal 0090 (v0.246): `owned` is NOT an ArgModifiers
// alternative; `M(owned x)` is a parse error. Use `move x` (§22.3, §28.8.1).
BEZEL_TEST(owned_marker, bare_owned_argument_rejected_spec_a4_0090) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  var x := new T(); s.Add(owned x); "
        "end M; end C;"));
    // Parse error: invalid use of 'owned' at argument position (§A.4).
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// per §A.4 / proposal 0090 (v0.246): `owned` is NOT an ArgModifiers
// alternative; `name := owned x` is a parse error. Use `name := move x`.
BEZEL_TEST(owned_marker, named_owned_argument_rejected_spec_a4_0090) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  var x := new T(); s.Add(item := owned x); "
        "end M; end C;"));
    // Parse error on `owned`; the named-arg form is also invalid since
    // `owned` is not an ArgModifiers alternative at all.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// SEMA — accept/reject matrix (§22.3, v0.169).
// ---------------------------------------------------------------------------

// ACCEPT: a named owned LOCAL marked `move` transfers into the owned sink.
BEZEL_TEST(owned_marker, named_owned_local_marked_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  var x := new T(); s.Add(move x); "
        "end M; end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ACCEPT: an `owned` PARAMETER marked `move` forward-transfers into the sink.
BEZEL_TEST(owned_marker, owned_param_marked_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink, owned p: T) "
        "  s.Add(move p); "
        "end M; end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// REJECT: the bare named owned binding — missing marker — is an error naming
// the fix (`write \`M(move x)\``; v0.286 §22.3; the v0.169 spelling was
// `M(owned x)`, fix #3783).
BEZEL_TEST(owned_marker, bare_named_owned_local_missing_marker_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  var x := new T(); s.Add(x); "
        "end M; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "this call consumes `x`"));
    BEZEL_EXPECT_TRUE(errorSays(r, "write `M(move x)`"));
}

// REJECT: a bare `owned` PARAMETER without the marker is likewise missing it.
BEZEL_TEST(owned_marker, bare_owned_param_missing_marker_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink, owned p: T) "
        "  s.Add(p); "
        "end M; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "this call consumes `p`"));
}

// ACCEPT: a `new` expression (fresh producer) stays UNMARKED.
BEZEL_TEST(owned_marker, fresh_new_unmarked_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  s.Add(new T()); "
        "end M; end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ACCEPT: a call returning `owned T` (fresh producer) stays UNMARKED.
BEZEL_TEST(owned_marker, fresh_owned_call_unmarked_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  s.Add(s.MakeOwned()); "
        "end M; end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// REJECT: the marker on a `new` expression (fresh producer) is an error.
// per §A.4 / proposal 0090 (v0.246): `owned` is not an ArgModifiers
// alternative, so the error is at parse time (invalid `owned`), not sema.
BEZEL_TEST(owned_marker, marker_on_new_expr_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  s.Add(owned new T()); "
        "end M; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The sema "fresh producer" check is unreachable — parse rejects first.
}

// REJECT: the marker on an owned-returning call (fresh producer) is an error.
// per §A.4 / proposal 0090 (v0.246): `owned` is not an ArgModifiers
// alternative, so the error is at parse time (invalid `owned`), not sema.
BEZEL_TEST(owned_marker, marker_on_owned_call_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  s.Add(owned s.MakeOwned()); "
        "end M; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The sema "fresh producer" check is unreachable — parse rejects first.
}

// REJECT: the marker on a BORROW (a borrowed parameter) is an error — the
// existing §28.8 borrow diagnostic names why (a borrow is owned elsewhere).
// per §A.4 / proposal 0090 (v0.246): `owned` is not an ArgModifiers
// alternative, so the error is at parse time (invalid `owned`), not sema.
BEZEL_TEST(owned_marker, marker_on_borrow_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink, b: T) "
        "  s.Add(move b); "
        "end M; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The sema "borrow" check is unreachable — parse rejects first.
}

// REJECT: the marker on a NON-`owned` parameter position is an error naming it.
// per §A.4 / proposal 0090 (v0.246): `owned` is not an ArgModifiers
// alternative, so the error is at parse time (invalid `owned`), not sema.
BEZEL_TEST(owned_marker, marker_on_non_owned_position_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  var x := new T(); s.Peek(move x); x.Dispose(); "
        "end M; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The sema "does not take ownership" check is unreachable — parse rejects first.
}

// ACCEPT (constructor): a named owned local marked with `move` into an `owned` ctor param.
BEZEL_TEST(owned_marker, ctor_owned_param_marked_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class Box public constructor(owned t: T) using var h := move t; end constructor; end Box; "
        "class C public method M() "
        "  var x := new T(); using var b := new Box(move x); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// REJECT (constructor): the bare named owned local — missing marker.
BEZEL_TEST(owned_marker, ctor_bare_named_owned_local_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class Box public constructor(owned t: T) using var h := move t; end constructor; end Box; "
        "class C public method M() "
        "  var x := new T(); using var b := new Box(x); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "this call consumes `x`"));
}

// fix #4726 (§22.3, v0.315): REJECT (LocalDecl-initializer): `move new T()`
// into an `owned` ctor formal is a redundant marker — a `new` expression
// is a fresh producer and the spec says "no live binding is invalidated,
// so there is nothing to announce". Before this fix `recordMovesIn`'s NewExpr
// arm accepted this silently; `handleDischarge`'s NewExpr arm already rejected
// it (#4721, the method-call twin). Both paths are now consistent.
BEZEL_TEST(owned_marker, ctor_move_fresh_new_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class Box public constructor(owned t: T) using var h := move t; end constructor; end Box; "
        "class C public method M() "
        "  using var b := new Box(move new T()); " // spurious move on fresh producer
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "fresh producer"));
}

// GUARD (not a pin for #4726): the bare-statement `new Box(move new T())`
// form is rejected by `handleDischarge`'s NewExpr arm — the #4721 fix already
// covered it. This fixture is a NON-REGRESSION guard: it confirms that
// `recordMovesIn`'s fix does not inadvertently bypass the handleDischarge
// path. The #4726 pin is `ctor_move_fresh_new_rejected` above.
// per §22.3, v0.315
BEZEL_TEST(owned_marker, ctor_move_fresh_new_bare_statement_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class Box public constructor(owned t: T) using var h := move t; end constructor; end Box; "
        "class C public method M() "
        "  new Box(move new T()); " // spurious move on fresh producer
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "fresh producer"));
}

// UNAFFECTED: `move x.f` of an owned field (a fresh producer) stays UNMARKED.
BEZEL_TEST(owned_marker, move_owned_field_unmarked_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class Sink public method Add(owned item: T) using var h := move item; end Add; end Sink; "
        "class C "
        "  private owned _f: T := new T(); "
        "  public method M(s: Sink) "
        "    s.Add(move this._f); "        // fresh producer: no marker
        "    this._f := new T(); "         // restore the field (§28.8.1)
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// UNAFFECTED: an `out owned var` inline declaration is the owned-OUT direction
// (the local RECEIVES ownership) — not a transfer-in, so it is not subject to
// the call-site marker (§22.4). The received value discharges via a `using var`.
BEZEL_TEST(owned_marker, out_owned_var_unaffected) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class Src public method Get(out owned r: T) r := new T(); end Get; end Src; "
        "class C public method M(s: Src) "
        "  s.Get(out var top); "
        "  using var h := move top; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8 exception-edge example ("build then take then restore"): the taken value
// `taken` is a named owned binding, so the forwarding call carries the `move` marker.
BEZEL_TEST(owned_marker, spec_28_8_exception_edge_taken_marked_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class Sink public method Take(owned t: T) using var h := move t; end Take; end Sink; "
        "class C "
        "  private owned _resource: T := new T(); "
        "  public method TransferSafe(s: Sink) "
        "    var fresh := new T(); "
        "    var taken := move this._resource; "
        "    this._resource := fresh; "               // restore before the call
        "    s.Take(move taken); "                    // §22.3 marker on named binding
        "  end TransferSafe; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// CODEGEN value test — the marked transfer disposes EXACTLY ONCE (surface-only:
// runtime behaviour is identical to the unmarked pre-v0.169 form). Uses a static
// live-instance counter; Main returns the residual live count, which is 0 iff
// every transferred Cell was disposed exactly once.
// ---------------------------------------------------------------------------

BEZEL_TEST(owned_marker, marked_transfer_disposes_once_value) {
    // Take(owned c) consumes and disposes c; Test forwards its owned param with
    // the marker. Two transfers (local→Test param, param→Take param), one Cell,
    // one destructor call → Cell.live == 0 at the end.
    std::string src =
        "module M; "
        "class Cell "
        "  public static live: int := 0; "
        "  public constructor() Cell.live := Cell.live + 1; end constructor; "
        "  destructor Cell.live := Cell.live - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Take(owned c: Cell) "
        "    using var u := move c; "
        "  end Take; "
        "  public static method Forward(owned c: Cell) "
        "    C.Take(move c); "                        // §22.3: forward the owned param
        "  end Forward; "
        "  public static method Main(): int "
        "    var x := new Cell(); "
        "    C.Forward(move x); "                      // §22.3: transfer the owned local
        "    return Cell.live; "                        // 0 — disposed exactly once
        "  end Main; "
        "end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// §30.10 / §22.3 (#1890): Task.FireAndForget(owned task: Task) is a builtin
// owned sink. A NAMED owned task local passed as `owned t` is the mandated
// §22.3 spelling; the unmarked form is a missing-marker error, not the pre-fix
// "would be dropped without disposal" one. A common async-producer helper.
// ---------------------------------------------------------------------------

// A module whose Main holds a named owned task local `t := Ping()` (an async
// call produces an owned Task, spec §5840). `body` is the FireAndForget call.
static std::string fafMod(const std::string& call) {
    return std::string(
        "module M; class C "
        "  public static async method Ping(): Task "
        "    await Task.Delay(1); "
        "  end Ping; "
        "  public static async method Main(): Task<int> "
        "    var t := Ping(); ") + call + " "
        "    return 0; "
        "  end Main; end C;";
}

// ACCEPT: `Task.FireAndForget(move t)` on a named owned task local — the §22.3
// mandated spelling. Before the fix the `owned` marker was spuriously rejected
// ("only valid on an argument to an `owned` parameter") because FireAndForget's
// owned parameter was not modeled as an owned sink.
BEZEL_TEST(owned_marker, fire_and_forget_owned_named_task_accepted_1890) {
    auto r = parseAndCheckOwnership(fafMod("Task.FireAndForget(move t);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// REJECT: `Task.FireAndForget(t)` unmarked — a named owned binding to an owned
// parameter REQUIRES the marker (§22.3). The error must be the missing-marker
// one naming the fix, not the pre-fix undischarged "would be dropped" error.
BEZEL_TEST(owned_marker, fire_and_forget_unmarked_named_task_missing_marker_1890) {
    auto r = parseAndCheckOwnership(fafMod("Task.FireAndForget(t);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "this call consumes `t`"));
    BEZEL_EXPECT_TRUE(errorSays(r, "write `M(move t)`"));
}

// ACCEPT: the fresh-producer form `Task.FireAndForget(Ping())` stays UNMARKED —
// no live binding is invalidated, so the marker must not be required (§22.3).
BEZEL_TEST(owned_marker, fire_and_forget_fresh_producer_unmarked_accepted_1890) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static async method Ping(): Task "
        "    await Task.Delay(1); "
        "  end Ping; "
        "  public static async method Main(): Task<int> "
        "    Task.FireAndForget(Ping()); "
        "    return 0; "
        "  end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// REJECT: the marker on the fresh-producer form `Task.FireAndForget(owned
// Ping())` is spurious — no named binding is left moved-from (§22.3).
// per §A.4 / proposal 0090 (v0.246): `owned` is not an ArgModifiers
// alternative, so the error is at parse time (invalid `owned`), not sema.
BEZEL_TEST(owned_marker, fire_and_forget_marker_on_fresh_producer_rejected_1890) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static async method Ping(): Task "
        "    await Task.Delay(1); "
        "  end Ping; "
        "  public static async method Main(): Task<int> "
        "    Task.FireAndForget(owned Ping()); "
        "    return 0; "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The sema "fresh producer" check is unreachable — parse rejects first.
}
