// Spec-derived tests for issue #430 — a SOUNDNESS hole: a catch-bound exception
// variable could escape its arm indirectly by being passed as a NON-OWNED
// constructor argument to an object that is then THROWN (or returned) from the
// handler.
//
// §31.1 "Exception ownership": "Inside a catch arm, the bound exception
// variable is a **borrow** of the in-flight exception; its source is the
// runtime, and its lifetime is the arm body's. The borrow may be passed to
// other methods (which further borrow for the duration of the call) but may
// not be assigned to an `owned` field, returned as `owned`, or captured by a
// closure that may outlive the arm."
// §28.3: "A borrowed local may not be assigned to any field of any object —
// neither owned nor non-owned."
//
// The escape pattern: a `Wrap` class holds a NON-OWNED `_inner: Exception?`
// field. Its constructor takes a non-owned borrow parameter `e: Exception`
// and stores it into `_inner`. If a catch handler does `throw new Wrap(ex)`,
// the Wrap escapes the handler. After the handler completes, the runtime
// disposes `ex`; Wrap._inner now dangles → use-after-free.
//
// PRE-EXISTING COVERAGE (#392, #404): the checker already rejects a catch
// borrow flowing into an OWNED field (directly or through an `owned` ctor
// param). The gap #430 closes is specifically the NON-OWNED ctor param path
// when the resulting object ESCAPES the arm (thrown/returned). Constructing
// a Wrap locally (not thrown/returned) must remain accepted.
//
// SOUNDNESS DISCIPLINE: every positive rejects a program §31.1 makes an
// error; every negative is a spec-LEGAL program that must be accepted. The
// cardinal risk is over-rejection:
//   - `throw ex;` (direct re-throw) MUST be accepted.
//   - Reading `ex.Message` / passing ex as a borrow MUST be accepted.
//   - `new Wrap(ex)` used locally (not thrown/returned) MUST be accepted.
//   - `throw new Wrap(ex.Message)` (string arg, not the borrow) MUST be
//     accepted.
//   - The already-rejected owned-field case MUST still be rejected.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// Wrap: a class with a NON-OWNED `_inner: Exception?` field. Its constructor
// stores a non-owned borrow parameter into `_inner`. The owned `_dummy` field
// makes Wrap disposable so `using var` in the no-escape test is valid. A
// `Helper(e: Exception)` method for the borrow-call control. `DoWork()` is
// the protected operation in the `try` body.
//
// The critical design: `_inner` is NOT owned. The existing #404 check catches
// `owned` constructor parameters; #430 catches the non-owned parameter case
// when the object escapes.
std::string wrapDecl() {
    return std::string(
        "module M; "
        "class Wrap "
        "  private owned _dummy: Exception?; "
        "  public _inner: Exception?; "
        "  public constructor(e: Exception) "
        "    this._inner := e; "
        "  end constructor; "
        "end Wrap; "
        "class C "
        "  public owned _f: Exception?; "
        "  private _text: string?; "
        "  private method Helper(e: Exception) end Helper; "
        "  private method DoWork() end DoWork; ");
}

} // namespace

// =====================================================================
// POSITIVE 1 — `throw new Wrap(ex)` escapes the catch borrow via a
//              NON-OWNED field of the thrown object (§31.1 / #430).
// =====================================================================

// Direct: `throw new Wrap(ex)` where Wrap's ctor stores ex in a non-owned
// field `_inner`. The thrown Wrap escapes the handler; ex is freed when
// the handler completes → Wrap._inner dangles. REJECTED. (§31.1 / #430.)
BEZEL_TEST(catch_borrow_via_ctor, throw_new_with_catch_borrow_arg_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrapDecl() +
        "  public method Handle() "
        "    try DoWork(); "
        "    catch var ex: Exception => throw new Wrap(ex); "
        "    end; "
        "  end Handle; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "caught exception `ex` may not be passed to a constructor"));
}

// =====================================================================
// POSITIVE 2 — `return new Wrap(ex)` escapes the catch borrow via the
//              returned object (§31.1 / #430).
// =====================================================================

// Return form: `return new Wrap(ex)` from a catch arm. The returned Wrap
// escapes the arm; ex is freed when the arm completes → Wrap._inner dangles.
// REJECTED. (§31.1 / #430.)
BEZEL_TEST(catch_borrow_via_ctor, return_new_with_catch_borrow_arg_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrapDecl() +
        "  public method Handle(): Wrap "
        "    try DoWork(); "
        "    catch var ex: Exception => return new Wrap(ex); "
        "    end; "
        "    return new Wrap(new Exception()); "
        "  end Handle; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "caught exception `ex` may not be passed to a constructor"));
}

// =====================================================================
// NEGATIVES (must NOT be rejected) — the §31.1-legal shapes.
// =====================================================================

// CONTROL: `new Wrap(ex)` used LOCALLY and NOT escaped (used+dropped within
// the catch arm body via `using var`). The object's lifetime is bounded by
// the arm body, so it cannot outlive ex. No escape → no §31.1 violation.
// ACCEPTED. (§31.1 / #430 no-over-rejection.)
BEZEL_TEST(catch_borrow_via_ctor, local_wrap_not_escaping_clean_31_1) {
    auto r = parseAndCheckOwnership(wrapDecl() +
        "  public method Handle() "
        "    try DoWork(); "
        "    catch var ex: Exception => do "
        "      using var w := new Wrap(ex); "
        "      Helper(ex); "
        "    end; "
        "    end; "
        "  end Handle; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §31.1 / §17.12 (v0.182, #1569): `throw ex;` re-raising the caught binding is
// a compile error — the binding is a borrow and the throw operand must be
// OWNED; the bare re-raise `throw;` is the correct form. REJECTED (not via the
// new-object ctor path — directly as a borrowed operand).
BEZEL_TEST(catch_borrow_via_ctor, rethrow_borrowed_binding_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrapDecl() +
        "  public method Handle() "
        "    try DoWork(); "
        "    catch var ex: Exception => throw ex; "
        "    end; "
        "  end Handle; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "the thrown value must be an OWNED exception"));
}

// CONTROL (§31.1): reading `ex.Message` (a value/string projection) and
// passing ex as a borrow to a helper method are the documented legal uses.
// No object that escapes is constructed from ex. ACCEPTED.
BEZEL_TEST(catch_borrow_via_ctor, read_message_and_borrow_call_clean_31_1) {
    auto r = parseAndCheckOwnership(wrapDecl() +
        "  public method Handle() "
        "    try DoWork(); "
        "    catch var ex: Exception => do "
        "      this._text := ex.Message; "
        "      Helper(ex); "
        "    end; "
        "    end; "
        "  end Handle; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: `throw new Wrap(ex.Message)` passes a VALUE PROJECTION (string)
// as the constructor argument — not the exception instance itself. Wrap's
// constructor takes a non-owned parameter but the argument is a string copy,
// not the exception borrow. ACCEPTED.
// (This test verifies we only reject bare catch-borrow identifiers, not
// member-access value projections.)
BEZEL_TEST(catch_borrow_via_ctor, throw_new_with_message_string_arg_clean_31_1) {
    // We need a Wrap variant that takes a string arg; reuse Wrap2-like class.
    auto r = parseAndCheckOwnership(std::string(
        "module M; "
        "class WrapStr "
        "  private _t: string?; "
        "  public constructor(t: string) "
        "    this._t := t; "
        "  end constructor; "
        "end WrapStr; "
        "class C "
        "  private method DoWork() end DoWork; "
        "  public method Handle() "
        "    try DoWork(); "
        "    catch var ex: Exception => throw new WrapStr(ex.Message); "
        "    end; "
        "  end Handle; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// REGRESSION GUARD: storing the catch binding into an OWNED field (the
// pre-existing #392 / #404 case) must still be rejected. REJECTED.
BEZEL_TEST(catch_borrow_via_ctor, regression_store_to_owned_field_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrapDecl() +
        "  public method Handle() "
        "    try DoWork(); "
        "    catch var ex: Exception => this._f := ex; "
        "    end; "
        "  end Handle; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "caught exception `ex` may not be assigned to a field"));
}

// REGRESSION GUARD: the bare re-throw `throw;` (null operand) is the one
// exception §31.2.4 permits. ACCEPTED.
BEZEL_TEST(catch_borrow_via_ctor, bare_rethrow_stmt_clean_31_2_4) {
    auto r = parseAndCheckOwnership(wrapDecl() +
        "  public method Handle() "
        "    try DoWork(); "
        "    catch var ex: Exception => throw; "
        "    end; "
        "  end Handle; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
