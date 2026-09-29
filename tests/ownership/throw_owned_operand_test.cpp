// Spec-derived tests for the §31.1 / §17.12 throw-operand OWNERSHIP rule
// (issue #1569; docs v0.182).
//
// §31.1 (v0.182): "The operand is consumed as an owned value under the ownership
// rules below, so a *borrowed* class reference is not a valid operand: to
// re-raise the exception being handled, use the bare re-raise (§31.2.4); to
// propagate a caught exception's information, chain a fresh instance through the
// snapshot constructor (§31.4)." Inside a catch arm "the bound exception
// variable ... is a borrow of the in-flight exception; its ... lifetime is the
// arm body's" and "cannot escape the arm."
//
// The throw operand is therefore an OWNED sink exactly like an `owned`
// parameter (§28.8): the owned producers §31.1 admits — a fresh `new`, an owned
// local or `owned` parameter transferred out, a `move`d owned value — are
// accepted; a borrowed operand — a catch-arm binding re-thrown, a non-`owned`
// parameter, a field read, `this` — is a compile error (the borrow's owner and
// the runtime would both dispose it: a use-after-free / double-dispose).
//
// The operand TYPE rule (must be a class, not a primitive/string/Object/
// interface/record/enum) is the sema rule, tested in
// tests/sema/throw_operand_type_test.cpp.
//
// SOUNDNESS DISCIPLINE: every negative rejects a program §31.1 makes an error;
// every positive is a structurally-similar but spec-LEGAL program that must be
// accepted. Over-rejection is the cardinal sin — the owned producers and the two
// spec-blessed re-raise forms (bare `throw;` and the cause-chained fresh
// instance) must stay accepted.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// `AppError` — an `Exception`-derived class (the owned exception payload).
// `T.W()` — the protected operation whose call inside a `try` produces the
// in-flight exception a catch arm binds.
std::string wrap(const std::string& method) {
    return std::string(
        "module M; "
        "class AppError : Exception "
        "  public constructor() base(\"e\"); end constructor; "
        "end AppError; "
        "class T "
        "  private method W() throw new Exception(\"boom\"); end W; ") + method +
        "end T;";
}

} // namespace

// ── REJECTED — a borrowed operand is not a valid throw operand (§31.1) ─────────

// §31.1 / §17.12 (#1569): re-throwing the caught binding `throw e;` — `e` is a
// BORROW of the in-flight exception whose owner is the runtime; the throw
// operand is consumed as OWNED, so the borrowed operand is rejected. The correct
// re-raise is the bare `throw;`. REJECTED.
BEZEL_TEST(throw_owned_operand, rethrow_caught_binding_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try W(); "
        "  catch var e: Exception => throw e; "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "the thrown value must be an OWNED exception"));
}

// §31.1 (#1569): throwing a non-`owned` (borrowed) parameter — its owner is the
// caller's frame; consuming it as an owned throw operand is a double-dispose.
// REJECTED.
BEZEL_TEST(throw_owned_operand, throw_borrowed_param_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Re(p: Exception) "
        "  throw p; "
        "end Re;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "the thrown value must be an OWNED exception"));
}

// ── ACCEPTED — owned producers and the spec-blessed re-raise forms ────────────

// §31.1: a fresh `new` is the canonical owned producer — ownership transfers to
// the runtime's exception machinery. ACCEPTED.
BEZEL_TEST(throw_owned_operand, throw_fresh_new_ok_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  throw new AppError(); "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §31.1: an owned local transferred out by a bare identifier `throw ex;` is a
// valid owned operand (the §28.3 auto-drop must-use warning may fire, but it is
// not an error). ACCEPTED.
BEZEL_TEST(throw_owned_operand, throw_owned_local_ok_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  var ex := new AppError(); "
        "  throw ex; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §31.1: `throw ex;` of an owned local is an owned transfer out of the local.
// ACCEPTED. Spelled marker-free per §28.3 K3.13 / K3.8 (v0.307): `throw` is a
// marker-free consuming position (the keyword announces), so `throw move ex;`
// is now rejected as a redundant marker; the §31.1 property this row pins —
// that an owned LOCAL is a valid throw operand — is unchanged.
BEZEL_TEST(throw_owned_operand, throw_move_owned_local_ok_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  var ex := new AppError(); "
        "  throw ex; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §31.1: an `owned` parameter transferred out by `throw p;` is a valid owned
// operand (the callee took ownership). ACCEPTED.
BEZEL_TEST(throw_owned_operand, throw_owned_param_ok_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Re(owned p: Exception) "
        "  throw p; "
        "end Re;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §31.2.4: the bare re-raise `throw;` transfers the in-flight exception back to
// the runtime — the spec-blessed way to re-raise the caught instance. ACCEPTED.
BEZEL_TEST(throw_owned_operand, bare_reraise_ok_31_2_4) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try W(); "
        "  catch var e: Exception => throw; "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §31.1 / §31.4: propagating a caught exception's information by chaining a
// FRESH instance through the snapshot constructor — `throw new Exception(
// e.Message, e)`. The fresh `new` is owned; `e.Message` is a value copy and `e`
// is a borrow passed for the constructor's duration (not retained as owned).
// ACCEPTED.
BEZEL_TEST(throw_owned_operand, throw_fresh_cause_chained_ok_31_4) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try W(); "
        "  catch var e: Exception => throw new Exception(e.Message, e); "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── §31.1 / §31.2.1 (issue #1591): the owned-operand rule inside a catch ──────
// arm's `when` GUARD. A guard is an ordinary expression position, and §31.2.1
// permits a guard to itself `throw` — diverging supersedes the in-flight
// exception and propagates, exactly like a `throw` in any other expression
// position (§31.1 states the operand rule for "any expression position" with
// no guard carve-out). Before the fix, flow::CFG's `visitTry` built a catch
// arm's CFG entry block from the arm's BODY only — the guard expression was
// never represented in the CFG at all, so the ownership pass's per-block walk
// never reached it and `checkThrowOperandNotBorrowed` never fired there, even
// though the identical borrowed operand in the arm BODY was already rejected
// (`rethrow_caught_binding_rejected_31_1` above). These pin the corrected
// guard-position behavior to match body-position exactly: REJECT a borrowed
// operand, ACCEPT an owned one.

// REJECTED: `catch var e: Exception when throw e => ...` re-throws the
// caught binding FROM THE GUARD — `e` is a borrow of the in-flight exception
// (§31.1), whether thrown from the guard or the body. Before the fix this
// compiled with no diagnostic (issue #1591's repro).
BEZEL_TEST(throw_owned_operand, throw_borrowed_binding_in_guard_rejected_31_1_1591) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try W(); "
        "  catch var e: Exception when throw e => return; "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "the thrown value must be an OWNED exception"));
}

// ACCEPTED: a fresh `new` thrown FROM THE GUARD is the canonical owned
// producer (§31.1) — the guard diverges and supersedes the in-flight
// exception (§31.2.1). Pins that closing the borrowed-operand gap does not
// over-reject a spec-legal guard-position throw.
BEZEL_TEST(throw_owned_operand, throw_fresh_new_in_guard_ok_31_1_1591) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try W(); "
        "  catch var e: Exception when throw new AppError() => return; "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
