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
// A.7 grammar: `ArgModifiers ::= ... | "owned"`; ref/out/owned arguments are
// positional-only (the named `name := expr` form is for plain values).

#include <string>
#include <string_view>

#include "support/codegen_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;
using bezel::test::runJit;

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
// PARSER (A.7): a bare `owned` argument modifier parses.
// ---------------------------------------------------------------------------

// A.7 `ArgModifiers ::= ... | "owned"`: `M(owned x)` parses (no syntax error);
// the argument binds positionally. A named `owned` form is not derivable.
BEZEL_TEST(owned_marker, parses_bare_owned_argument_spec_a7) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  var x := new T(); s.Add(owned x); "
        "end M; end C;"));
    // No PARSE error (a syntax error would mention "expected"); ownership accepts.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A.7 note: `ref`/`out`/`owned` arguments are positional-only. The named form
// `name := owned x` is not admitted (the named form is for plain values).
BEZEL_TEST(owned_marker, named_owned_argument_rejected_spec_a7) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  var x := new T(); s.Add(item := owned x); "
        "end M; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// SEMA — accept/reject matrix (§22.3, v0.169).
// ---------------------------------------------------------------------------

// ACCEPT: a named owned LOCAL marked `owned` transfers into the owned sink.
BEZEL_TEST(owned_marker, named_owned_local_marked_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  var x := new T(); s.Add(owned x); "
        "end M; end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ACCEPT: an `owned` PARAMETER marked `owned` forward-transfers into the sink.
BEZEL_TEST(owned_marker, owned_param_marked_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink, owned p: T) "
        "  s.Add(owned p); "
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
BEZEL_TEST(owned_marker, marker_on_new_expr_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  s.Add(owned new T()); "
        "end M; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "fresh producer"));
}

// REJECT: the marker on an owned-returning call (fresh producer) is an error.
BEZEL_TEST(owned_marker, marker_on_owned_call_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  s.Add(owned s.MakeOwned()); "
        "end M; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "fresh producer"));
}

// REJECT: the marker on a BORROW (a borrowed parameter) is an error — the
// existing §28.8 borrow diagnostic names why (a borrow is owned elsewhere).
BEZEL_TEST(owned_marker, marker_on_borrow_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink, b: T) "
        "  s.Add(owned b); "
        "end M; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow"));
}

// REJECT: the marker on a NON-`owned` parameter position is an error naming it.
BEZEL_TEST(owned_marker, marker_on_non_owned_position_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  var x := new T(); s.Peek(owned x); x.Dispose(); "
        "end M; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "does not take ownership"));
}

// ACCEPT (constructor): a named owned local marked into an `owned` ctor param.
BEZEL_TEST(owned_marker, ctor_owned_param_marked_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class Box public constructor(owned t: T) using var h := move t; end constructor; end Box; "
        "class C public method M() "
        "  var x := new T(); using var b := new Box(owned x); "
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
        "  s.Get(out owned var top); "
        "  using var h := move top; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8 exception-edge example ("build then take then restore"): the taken value
// `taken` is a named owned binding, so the forwarding call carries the marker.
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
        "    s.Take(owned taken); "                    // §22.3 marker on named binding
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
        "    C.Take(owned c); "                        // §22.3: forward the owned param
        "  end Forward; "
        "  public static method Main(): int "
        "    var x := new Cell(); "
        "    C.Forward(owned x); "                      // §22.3: transfer the owned local
        "    return Cell.live; "                        // 0 — disposed exactly once
        "  end Main; "
        "end C;";
    auto r = runJit(src);
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

// ACCEPT: `Task.FireAndForget(owned t)` on a named owned task local — the §22.3
// mandated spelling. Before the fix the `owned` marker was spuriously rejected
// ("only valid on an argument to an `owned` parameter") because FireAndForget's
// owned parameter was not modeled as an owned sink.
BEZEL_TEST(owned_marker, fire_and_forget_owned_named_task_accepted_1890) {
    auto r = parseAndCheckOwnership(fafMod("Task.FireAndForget(owned t);"));
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
    BEZEL_EXPECT_TRUE(errorSays(r, "fresh producer"));
}

// ---------------------------------------------------------------------------
// fix #4721 (§22.3, v0.307): the MOVE-EXPRESSION marker. Proposal 0090
// respelled the call-site marker from the (now-removed) `owned` ArgModifiers
// keyword — still pinned above, since the parser continues to accept it
// unchanged — to `move x`, an identifier-operand MoveExpr (A.7/A.4): "No
// `owned` alternative exists ... the call-site transfer marker is part of the
// argument EXPRESSION." The SAME admission rule §22.3 states applies to the
// new spelling: "writing it on a borrow, a fresh producer, or a non-`owned`
// parameter position is likewise an error." Before this fix, nothing checked
// a `move`-wrapped argument's POSITION at all — only the dead `a.modifier ==
// "owned"` spelling was gated — so both shapes below compiled clean.
// ---------------------------------------------------------------------------

// REJECT (rule-corpus s22_3_0007): `move` on a NEW EXPRESSION (a fresh
// producer) fed to an owned parameter is redundant — a fresh producer stays
// unmarked; no live binding is invalidated.
BEZEL_TEST(owned_marker, move_marker_on_new_expr_rejected_s22_3_0007) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  s.Add(move new T()); "
        "end M; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "fresh producer"));
}

// REJECT (§22.3, same rule, the owned-returning-call fresh-producer shape):
// `move` on a call returning `owned T` is likewise redundant.
BEZEL_TEST(owned_marker, move_marker_on_owned_call_rejected_4721) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  s.Add(move s.MakeOwned()); "
        "end M; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "fresh producer"));
}

// REJECT (rule-corpus s22_3_0009): `move` at a NON-owned parameter position
// is an error naming the borrow default — an owned expression may not feed a
// borrow slot.
BEZEL_TEST(owned_marker, move_marker_on_non_owned_position_rejected_s22_3_0009) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  var x := new T(); s.Peek(move x); x.Dispose(); "
        "end M; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "does not take ownership"));
}

// ACCEPT (control): the legitimate spelling — `move` on a NAMED OWNED
// BINDING fed to an owned parameter — still transfers cleanly. Unaffected by
// this fix: `a.value` is an IdentifierExpr operand, handled by the existing
// (unchanged) `checkOwnedArgSource`/`markMovedByName` path.
BEZEL_TEST(owned_marker, move_marker_named_owned_local_still_accepted_4721) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  var x := new T(); s.Add(move x); "
        "end M; end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// REGRESSION GUARD (§27.1 / §28.1, v0.307): `move` at a call through a
// FUNCTION-TYPED VALUE whose parameter slot is declared `owned` must NOT be
// rejected. An earlier draft of this fix gated the new check on plain
// `!ownedSink`, which reads false for a function-typed callee too — not
// because the slot is non-owned, but because `isImplicitOwned` /
// `builtinOwnedSinkArg` only resolve a Method/Constructor/ChoiceCase SYMBOL
// callee and never look at a function VALUE's own declared parameter
// modifiers. Measured regression (corpus s27_1_0003 / s28_1_0006, both
// `expect: accept`) before the `discFormals.valid()` gate was added.
BEZEL_TEST(owned_marker, move_marker_through_owned_function_type_slot_accepted_4721) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  private _consume: (owned T) -> (); "
        "  public constructor() "
        "    _consume := lambda(owned t: T) => t.Dispose(); "
        "  end constructor; "
        "  public method Run(owned w: T) "
        "    _consume(move w); "
        "  end Run; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
