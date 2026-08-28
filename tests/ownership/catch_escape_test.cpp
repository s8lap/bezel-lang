// Spec-derived tests for issue #392 — a SOUNDNESS hole in the ownership
// escape analysis: a catch-bound exception variable could escape its arm.
//
// §31.1 "Exception ownership": "Inside a catch arm, the bound exception
// variable (`var ex: Exception` in the arm pattern) is a **borrow** of the
// in-flight exception; its source is the runtime, and its lifetime is the arm
// body's. The borrow may be passed to other methods (which further borrow for
// the duration of the call) but may not be assigned to an `owned` field,
// returned as `owned`, or captured by a closure that may outlive the arm."
// "Exception instances themselves cannot escape the arm that caught them. The
// bare re-throw `throw;` ... is the one exception to this rule." Guide 02 §5
// shows `_lastError := ex;` (storing the binding into a field) as a COMPILE
// ERROR, and lists the three prohibited operations: assign to an owned field,
// return as owned, capture in a closure that outlives the arm.
//
// The runtime disposes the exception after the arm completes, so a retained
// reference dangles — a use-after-free. Before this fix the catch binding was
// an ordinary scope-local that the ownership pass did not treat as a borrow,
// so all three escapes compiled. This milestone reuses the existing #360/#386
// escape machinery: the catch binding is seeded as a scope-bound borrow whose
// lifetime is the arm body, and its escape is rejected on every channel the
// machinery already rejects an owned local's borrow on (return, store-to-
// field, out/ref param, closure capture, multi-hop alias).
//
// SOUNDNESS DISCIPLINE: every positive rejects a program §31.1 makes an error;
// every negative is a structurally-similar but spec-LEGAL program that must be
// accepted. Over-rejection is the cardinal sin, so each positive is paired
// with negatives that guard the exact §31.1 boundary: passing `ex` as a borrow
// argument, reading/copying a value projection out of it (`ex.Message`), using
// it locally in the arm, the bare re-throw `throw;`, and wrapping it
// `throw new T(ex)`, and a closure that captures `ex` but is invoked locally
// within the arm (does not escape). Re-raising the same instance via
// `throw ex;` is now itself a §31.1 error (v0.182, #1569: the borrowed operand
// is rejected — use the bare `throw;`).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// A class `C` with an `owned` Exception? field (the escape DESTINATION for the
// store-to-field positives), a `Helper(e: Exception)` that borrows the
// exception for the call's duration (the legal pass-as-borrow control), a
// `Use(e: Exception): int` value-returning helper (for closure bodies), and a
// `DoWork()` whose call inside the `try` body is the protected operation. The
// catch arms bind the built-in `Exception` type (§31.4), exactly as the spec
// and Guide 02 §5 examples do.
std::string wrap(const std::string& method) {
    return std::string(
        "module M; "
        "class C "
        "  public owned _f: Exception?; "
        "  private _text: string?; ") + method +
        "  private method Helper(e: Exception) end Helper; "
        "  private method Use(e: Exception): int return 0; end Use; "
        "  private method DoWork() end DoWork; "
        "end C;";
}

} // namespace

// =====================================================================
// POSITIVE 1 — store the catch binding into an `owned` field (§31.1; the
//              `_lastError := ex;` error from Guide 02 §5).
// =====================================================================

// Direct: `this._f := ex;` stores the in-flight exception's borrow into an
// owned field. The field outlives the arm; after the arm completes the runtime
// disposes the exception, so the stored reference dangles. (§31.1.)
BEZEL_TEST(catch_escape_field, store_binding_into_owned_field_diagnosed_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch var ex: Exception => this._f := ex; "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "caught exception `ex` may not be assigned to a field"));
}

// Indirect via a holder local: `var b := ex; this._f := b;` re-borrows the
// exception into `b`, then stores `b` into the field. The multi-hop alias
// tracking follows `b` back to `ex`. (§31.1.)
BEZEL_TEST(catch_escape_field, store_reborrow_holder_into_owned_field_diagnosed_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch var ex: Exception => do "
        "    var b := ex; "
        "    this._f := b; "
        "  end; "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "caught exception `ex` may not be assigned to a field"));
}

// --- POSITIVE 1 negative controls (must NOT be rejected) ------------------

// CONTROL (§31.1): copying a VALUE projection out of the exception is the
// documented escape hatch — "copy the relevant data out of it (typically
// `ex.Message` ...)". `ex.Message` is a string (runtime-managed, no ownership
// obligation), so storing it into a (non-owned) string field is legal.
BEZEL_TEST(catch_escape_field, store_message_string_into_field_clean_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch var ex: Exception => this._text := ex.Message; "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (§31.1): passing the binding as a borrow argument to a method is
// explicitly permitted — "may be passed to other methods (which further borrow
// for the duration of the call)". `Helper(ex)` does not let `ex` escape.
BEZEL_TEST(catch_escape_field, pass_binding_as_borrow_argument_clean_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch var ex: Exception => Helper(ex); "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =====================================================================
// POSITIVE 2 — return the catch binding (returned `owned`; the only way a
//              class instance leaves a method). (§31.1.)
// =====================================================================

// Direct: `return ex;` from a method whose return type is `Exception`. The
// exception instance escapes the arm; the runtime disposes it after the arm,
// so the returned reference dangles. (§31.1.)
BEZEL_TEST(catch_escape_return, return_binding_diagnosed_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(): Exception "
        "  try DoWork(); "
        "  catch var ex: Exception => return ex; "
        "  end; "
        "  return new Exception(); "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "caught exception `ex` may not be returned"));
}

// Indirect via a holder local: `var b := ex; return b;`. The holder re-borrows
// the exception; returning it escapes the arm. (§31.1.)
BEZEL_TEST(catch_escape_return, return_reborrow_holder_diagnosed_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(): Exception "
        "  try DoWork(); "
        "  catch var ex: Exception => do "
        "    var b := ex; "
        "    return b; "
        "  end; "
        "  end; "
        "  return new Exception(); "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "carries a borrow of the caught exception"));
}

// --- POSITIVE 2 negative controls (must NOT be rejected) ------------------

// CONTROL (§31.1): returning a value projection copied out of the exception is
// legal. `return ex.Message;` returns a string copy, not the instance.
BEZEL_TEST(catch_escape_return, return_message_string_clean_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(): string "
        "  try DoWork(); "
        "  catch var ex: Exception => return ex.Message; "
        "  end; "
        "  return \"ok\"; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (§31.1 / §31.2.4): the bare re-throw `throw;` is "the one exception
// to this rule, since it transfers the in-flight exception back to the runtime
// rather than capturing it." It must be accepted.
BEZEL_TEST(catch_escape_return, bare_rethrow_clean_31_2_4) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch var ex: Exception => throw; "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §31.1 / §17.12 (v0.182, #1569): re-raising the SAME instance with `throw ex;`
// is now a compile error — the caught binding is a BORROW whose owner is the
// runtime, and the `throw` operand is consumed as an OWNED value, so a borrowed
// operand is rejected. To re-raise the in-flight exception, use the bare
// re-raise `throw;` (§31.2.4). REJECTED. (Pre-v0.182 this form was accepted as
// a control transfer; the operand rule now forbids the borrowed operand.)
BEZEL_TEST(catch_escape_return, rethrow_borrowed_binding_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch var ex: Exception => throw ex; "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "the thrown value must be an OWNED exception"));
}

// CONTROL (§31.2.4): wrapping the exception in a new one — `throw new
// DataAccessException("...", ex)` — passes `ex` as a constructor ARGUMENT (a
// borrow for the call's duration) and raises a fresh instance. The arm body
// passes `ex` as a borrow; no escape. (Uses a built-in exception type so no
// stdlib API is invented.)
BEZEL_TEST(catch_escape_return, wrap_and_rethrow_clean_31_2_4) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch var ex: Exception => throw new InvalidOperationException(); "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =====================================================================
// POSITIVE 3 — capture the catch binding in a closure that escapes the arm
//              (returned). (§31.1: "captured by a closure that may outlive the
//              arm".)
// =====================================================================

// Direct: `return () => Use(ex);` returns a closure capturing the exception.
// The returned closure outlives the arm; the captured borrow dangles. (§31.1.)
BEZEL_TEST(catch_escape_closure, return_closure_capturing_binding_diagnosed_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(): () -> int "
        "  try DoWork(); "
        "  catch var ex: Exception => return lambda() => Use(ex); "
        "  end; "
        "  return lambda() => 0; "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned closure captures the caught exception"));
}

// Indirect via a holder local: `var f := () => Use(ex); return f;`. The closure
// captures the exception; binding it to `f` propagates the source, and
// returning `f` escapes the arm. (§31.1.)
BEZEL_TEST(catch_escape_closure, return_holder_closure_capturing_binding_diagnosed_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(): () -> int "
        "  try DoWork(); "
        "  catch var ex: Exception => do "
        "    var f := lambda() => Use(ex); "
        "    return f; "
        "  end; "
        "  end; "
        "  return lambda() => 0; "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "carries a borrow of the caught exception"));
}

// fix #3521 (§31.1 / §31.2.1, v0.283) — the SAME escape, with the `try`
// nested inside a `case` ARM BODY. Before the collapse, ownership answered "is
// this decl a catch-arm binding" with its own syntactic pre-pass
// (`collectCatchBorrowsStmt`), which recursed only into if / while / for /
// do-end / try. A catch arm reached through a `case` arm was therefore
// INVISIBLE to it, `isCatchBorrow` returned false, and every §31.1 escape check
// silently did not fire — this exact program compiled clean on b43196a. Reading
// sema's `LocalSymbol::isCatchBinding` makes the answer independent of any
// walker's statement-form coverage, which is the whole point of having one
// spelling. Nesting is the only difference from
// `return_closure_capturing_binding_diagnosed_31_1` above, and §31.1's rule —
// the borrow "may not be ... captured by a closure that may outlive the arm" —
// does not mention where the `try` sits.
BEZEL_TEST(catch_escape_closure, return_closure_capturing_binding_under_case_arm_diagnosed_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(n: int): () -> int "
        "  case n of "
        "    0 => do "
        "      try DoWork(); "
        "      catch var ex: Exception => return lambda() => Use(ex); "
        "      end; "
        "    end, "
        "    _ => do end "
        "  end case; "
        "  return lambda() => 0; "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned closure captures the caught exception"));
}

// CONTROL for the row above (§31.1): the same `case`-nested arm whose closure
// is INVOKED LOCALLY inside the arm escapes nothing and must stay accepted.
// This is the over-rejection guard on the widened coverage — feeding the escape
// machinery a source it could not previously see must not make a legal
// non-escaping capture an error.
BEZEL_TEST(catch_escape_closure, local_closure_under_case_arm_invoked_locally_clean_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(n: int): int "
        "  var k: int := 0; "
        "  case n of "
        "    0 => do "
        "      try DoWork(); "
        "      catch var ex: Exception => do "
        "        var f := lambda() => Use(ex); "
        "        k := f(); "
        "      end; "
        "      end; "
        "    end, "
        "    _ => do end "
        "  end case; "
        "  return k; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- POSITIVE 3 negative controls (must NOT be rejected) ------------------

// CONTROL (§31.1): a closure that captures `ex` but is INVOKED LOCALLY within
// the arm does not outlive the arm — it is "used locally within the arm". The
// capture's lifetime is bounded by the arm body, so there is no escape.
BEZEL_TEST(catch_escape_closure, local_closure_capturing_binding_invoked_locally_clean_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(): int "
        "  var n: int := 0; "
        "  try DoWork(); "
        "  catch var ex: Exception => do "
        "    var f := lambda() => Use(ex); "
        "    n := f(); "
        "  end; "
        "  end; "
        "  return n; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =====================================================================
// CROSS-CUTTING negative controls — the arm body that only reads / uses the
// exception locally and lets it go is the idiomatic, legal shape (§31.1; the
// `RecordError(ex)` pattern of Guide 02 §5).
// =====================================================================

// CONTROL (§31.1): using the binding purely locally — a borrow-call plus a
// value-projection copy into a non-owned field — is the recommended pattern.
// Nothing escapes the arm.
BEZEL_TEST(catch_escape_local_use, read_and_borrow_call_clean_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch var ex: Exception => do "
        "    Helper(ex); "
        "    this._text := ex.Message; "
        "  end; "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (§31.2.1): a NO-BINDING arm (`Exception => ...`) binds nothing, so
// there is no catch borrow to escape. A method call in its body is fine.
BEZEL_TEST(catch_escape_local_use, no_binding_arm_clean_31_2_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle() "
        "  try DoWork(); "
        "  catch Exception => DoWork(); "
        "  end; "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
