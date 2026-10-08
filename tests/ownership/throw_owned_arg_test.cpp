// Spec-derived tests for issue #406 — a §28.8 SOUNDNESS hole: the #395 owned-
// argument check (a borrow may not be passed where an `owned` value is required)
// was SKIPPED when the `new`/call is the operand of a `throw`.
//
// §28.8 (ownership transfer): "When a parameter is `owned`, the caller
// relinquishes ownership and may not use the argument after the call." A `new T`
// whose constructor takes an `owned` parameter is therefore a TRANSFER: the
// freshly-constructed `T` takes responsibility for disposing the value. Handing
// it a BORROW (a non-`owned` parameter, a Borrowed-state local, a bare owned-
// field read, or a catch-bound exception) is a double-free — the new object's
// owner AND the borrow's real owner both dispose it.
//
// §31.1 (throwing): "`new SomeException(...)` produces an owned class instance
// that the `throw` statement immediately consumes — ownership transfers to the
// runtime's exception machinery." The thrown `new` is constructed exactly as in
// any other position, so the constructor's owned-argument obligation is exactly
// the same. Nothing in §31.1 / §31.2 / §17.12 exempts a throw-operand `new` from
// the §28.8 transfer rule; the operand is an ordinary expression evaluated to an
// owned value that the throw then consumes.
//
// THE GAP: in the ownership pass the argument walker (`handleDischarge`, which
// runs `checkOwnedArgSource` over each `new`/call argument) was invoked for
// assignment / decl / expression-statement operands but NOT for a `new`/call
// that is the operand of a `throw` statement — `MethodOwnership::visitStmt` had
// no `ThrowStmt` case, so the thrown expression (and its arguments) was never
// walked. `throw new Wrap(b)` thus compiled while the equivalent
// `using var x := new Wrap(b)` was correctly rejected. #406 walks the thrown
// expression through the same arg path, closing the throw-operand sink.
//
// This also completes #404 for the throw-operand case: `catch e => throw new
// Wrap(e)` with an `owned` constructor argument retains the in-flight exception
// past the arm (§31.1: the binding is a borrow whose lifetime is the arm body;
// the runtime disposes it after the arm). Before #406 the throw-operand path
// skipped the classifier; now it is rejected like every other owned sink.
//
// SOUNDNESS DISCIPLINE: every negative rejects a program §28.8/§31.1 makes an
// error; every positive is a structurally-similar but spec-LEGAL program that
// must be accepted both before and after the fix. Over-rejection is the cardinal
// sin: a `move`d owned local, a fresh `new`, a value/string projection out of
// the exception, a plain `new SomeException("msg")`, and a bare re-throw of the
// in-flight exception are all legal owned producers / control transfers and
// must stay accepted.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// Type declarations shared by the throw-operand tests.
//
//  - `Inner`                          — a plain disposable class (the owned
//                                       payload for `Wrap`).
//  - `Wrap(owned c: Inner)`           — the owned-CONSTRUCTOR-arg sink for a
//                                       non-exception payload (the headline
//                                       borrowed-param case).
//  - `WrapE(owned c: Exception)`      — the owned-constructor-arg sink for the
//                                       catch-bound exception (#404 completion).
//  - `Wrap2(t: string)`              — a value/string-projection sink: the ctor
//                                       takes a borrow `string`, NOT an `owned`
//                                       class, so `e.Message` flowing in is a
//                                       copy, not the instance (§31.1 escape).
//  - `SomeException(t: string)`       — a user exception with a string ctor, for
//                                       the plain `throw new SomeException("m")`.
//  - `W()`                            — the protected operation in the `try`.
std::string wrap(const std::string& method) {
    return std::string(
        "module M; "
        "class Inner end Inner; "
        "class Wrap: Exception "
        "  private owned _c: Inner?; "
        "  public constructor(owned c: Inner) this._c := c; end constructor; "
        "end Wrap; "
        "class WrapE: Exception "
        "  private owned _c: Exception?; "
        "  public constructor(owned c: Exception) this._c := c; end constructor; "
        "end WrapE; "
        "class Wrap2: Exception "
        "  private _t: string?; "
        "  public constructor(t: string) this._t := t; end constructor; "
        "end Wrap2; "
        "class SomeException: Exception "
        "  private _t: string?; "
        "  public constructor(t: string) this._t := t; end constructor; "
        "end SomeException; "
        "class T "
        "  private method W() end W; ") + method +
        "end T;";
}

} // namespace

// =====================================================================
// NEGATIVES (must be REJECTED) — the throw-operand owned-arg sinks #406 closes.
// =====================================================================

// §28.8 / §31.1 — the headline #406 case: a borrowed parameter `b` passed to the
// `owned` constructor argument of a `new` that is the operand of a `throw`. The
// thrown `Wrap` takes ownership of a borrow → double-dispose when both `Wrap`
// and `b`'s real owner (the caller's frame) dispose it. Before #406 the throw-
// operand path skipped the check and this compiled. REJECTED.
BEZEL_TEST(throw_owned_arg, borrow_param_to_owned_ctor_arg_rejected_28_8) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(b: Inner) "
        "  throw new Wrap(b); "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow cannot be passed where an `owned` value is required"));
}

// §28.8 / §31.1 — the same borrow flowing in with an explicit `owned` call-site
// modifier on the argument. Still a borrow, still REJECTED.
BEZEL_TEST(throw_owned_arg, borrow_param_explicit_owned_modifier_rejected_28_8) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(b: Inner) "
        "  throw new Wrap(move b); "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow cannot be passed where an `owned` value is required"));
}

// §31.1 / §28.8 (#404 completion) — `catch e => throw new WrapE(e)`: the catch-
// bound exception is a borrow whose lifetime is the arm body (§31.1); the
// `owned` constructor argument retains it past the arm, and the runtime disposes
// the exception after the arm completes → use-after-free / double-dispose.
// Before #406 the throw-operand path skipped the classifier even though the
// non-throw form (`using var box := new WrapE(e)`, see catch_owned_sink_test)
// was already rejected by #404. REJECTED.
BEZEL_TEST(throw_owned_arg, catch_binding_to_owned_ctor_arg_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try this.W(); "
        "  catch var e: Exception => throw new WrapE(e); "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow cannot be passed where an `owned` value is required"));
}

// §31.1 / §28.8 — same as above with an explicit `move` on the catch binding.
// `move e` of a borrow is no more an owned value than the bare binding (the
// runtime, not this scope, owns it). REJECTED.
BEZEL_TEST(throw_owned_arg, catch_binding_move_to_owned_ctor_arg_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try this.W(); "
        "  catch var e: Exception => throw new WrapE(move e); "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow cannot be passed where an `owned` value is required"));
}

// =====================================================================
// POSITIVES (must be ACCEPTED) — legal owned producers / control transfers.
// Zero over-rejection: each passes BEFORE and AFTER the fix.
// =====================================================================

// §28.8.1 — `move ownedLocal` reads the owned local as an owned value,
// transferring ownership into the thrown `Wrap`'s `owned` constructor argument.
// A legal transfer; the local is left moved-from and not used again. ACCEPTED.
BEZEL_TEST(throw_owned_arg, move_owned_local_to_owned_ctor_arg_clean_28_8_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  var inner := new Inner(); "
        "  throw new Wrap(move inner); "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8 — a fresh `new Inner()` is an owned producer; nesting it as the `owned`
// constructor argument of the thrown `Wrap` is the canonical transfer. ACCEPTED.
BEZEL_TEST(throw_owned_arg, new_inner_to_owned_ctor_arg_clean_28_8) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  throw new Wrap(new Inner()); "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §31.1 — copying a value/string projection out of the caught exception into a
// fresh exception is the documented escape hatch ("copy the relevant data out of
// it, typically `ex.Message` ... into separate owned values"). `e.Message` is a
// string copy, not the instance; `Wrap2`'s constructor takes a borrow `string`,
// not an `owned` class, so nothing of the in-flight exception is retained.
// ACCEPTED.
BEZEL_TEST(throw_owned_arg, project_message_into_new_exception_clean_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try this.W(); "
        "  catch var e: Exception => throw new Wrap2(e.Message); "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §31.1 — the plain construct-and-throw: `throw new SomeException("msg")` raises
// a fresh owned instance whose only argument is a string literal (a borrow that
// is not a class). No owned-class borrow is sunk anywhere. ACCEPTED.
BEZEL_TEST(throw_owned_arg, plain_new_exception_literal_arg_clean_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  throw new SomeException(\"msg\"); "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §31.1 / §17.12 (v0.182, #1569) — re-raising the SAME instance with `throw e;`
// is a compile error: the caught binding `e` is a BORROW whose owner is the
// runtime, and the throw operand is consumed as an OWNED value, so a borrowed
// operand is rejected. The correct re-raise of the in-flight exception is the
// bare `throw;` (§31.2.4). REJECTED. (Pre-v0.182 this bare-identifier re-throw
// was accepted as a control transfer; the operand rule now forbids it.)
BEZEL_TEST(throw_owned_arg, rethrow_borrowed_binding_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try this.W(); "
        "  catch var e: Exception => throw e; "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "the thrown value must be an OWNED exception"));
}

// §31.2.4 — wrapping with a fresh no-arg exception: `throw new
// InvalidOperationException();` (a built-in nullary exception, §31.4). The catch
// binding is not sunk anywhere. ACCEPTED. (Guards that the new `ThrowStmt` walk
// does not spuriously flag an argumentless thrown `new`.)
BEZEL_TEST(throw_owned_arg, throw_new_no_arg_builtin_clean_31_2_4) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try this.W(); "
        "  catch var e: Exception => throw new InvalidOperationException(); "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8 — REGRESSION GUARD: a genuine owned value (`new Inner()`) flowing into
// the SAME `Wrap(owned c)` sink from a NON-throw context (`using var`) stays
// accepted. The #406 ThrowStmt walk must not disturb the legal `new`-expr
// transfer in any position. ACCEPTED.
BEZEL_TEST(throw_owned_arg, new_into_owned_ctor_arg_nonthrow_clean_28_8) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  using var w := new Wrap(new Inner()); "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
