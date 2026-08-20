// Ownership — fix #3075: §31.1 v0.243 makes the #430 catch-borrow-escape check
// structurally dead at every `throw new W(...)` site where `W` is exception-
// derived. The §31.2.4 / #811 cause-position carve-out was applied to ANY
// exception-typed argument when the constructed type was exception-derived —
// and §31.1 v0.243 makes every throw site fulfill that condition — so the
// carve-out swallowed the check at sites that have nothing to do with the
// snapshot-chaining `cause` slot.
//
// Spec (revision v0.243):
//   §31.1 "Exception ownership": "Inside a catch arm, the bound exception
//     variable is a **borrow** of the in-flight exception; its source is the
//     runtime, and its lifetime is the arm body's. The borrow may be passed
//     to other methods (which further borrow for the duration of the call)
//     but may not be assigned to an `owned` field, returned as `owned`, or
//     captured by a closure that may outlive the arm."
//   §31.2.4 "Bare re-raise": "to wrap the current exception, use
//     `throw expr` […] `throw new DataAccessException("query failed", ex),
//     // ex borrowed for the ctor call; snapshot taken, instance not retained
//     (§31.4)`"
//   §31.4 "Standard exception types": the base `Exception` constructor is
//     `public constructor(message: string, cause: Exception);` — `cause` is
//     the spec-defined marker for the snapshot-chaining slot; user subclasses
//     "chain by calling the base constructor: `public constructor(msg: string,
//     cause: Exception) base(msg, cause) end constructor;`".
//
// THE DEFECT. `MethodOwnership::catchBorrowAmongNonOwnedCtorArgs` checked
// ONLY "is the constructed type exception-derived AND is this argument
// exception-typed?" — true at every `throw new W(ex)` site given §31.1
// v0.243's operand-type rule — and skipped the #430 catch-borrow check for
// EVERY argument without asking whether the argument was in the spec-defined
// cause slot. MEASURED: `throw new Wrap(ex)` where `Wrap: Exception` and
// `Wrap`'s constructor takes a non-owned `e: Exception` parameter is silently
// accepted (the carve-out fires for the `ex` argument even though it lands in
// the `e` parameter, not the `cause` parameter). The throw escapes the
// catch arm with `ex` borrowed into a non-owned field of the thrown object
// → use-after-free when the runtime disposes `ex` after the arm completes.
//
// THE FIX. Narrow the carve-out to require the parameter at `pos` to be named
// `cause` (the spec-defined marker for the snapshot-chaining slot). An arg
// in any other parameter position falls through to the existing #430 check.
//
// These are diagnostic tests; the runtime consequence is what the existing
// `catch_borrow_escaping_via_ctor_test.cpp` documents (#430's regression
// suite — the throw side was RETARGETED at v0.243 because the rule that
// rejects it was stuck off, which is the bug #3075 names).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// Common prefix for all three test cases. `Wrap` is a class deriving from
// `Exception` (§31.1 v0.243 makes the operand-type rule pass for it). Its
// constructor takes a non-owned `e: Exception` parameter — name `e`, NOT
// `cause` — so the §31.2.4 carve-out, narrowed to require the cause-position
// parameter name, must NOT fire for an `ex` argument at the `e` position.
std::string wrapAsExc() {
    return std::string(
        "module M; "
        "class Wrap: Exception "
        "  private owned _dummy: Exception?; "
        "  public _inner: Exception?; "
        "  public constructor(e: Exception) "
        "    this._inner := e; "
        "  end constructor; "
        "end Wrap; "
        "class C "
        "  private method DoWork() end DoWork; "
        "  public method Handle() "
        "    try DoWork(); "
        "    catch var ex: Exception => ");
}

// §31.4 base `Exception` snapshot ctor: `constructor(message: string, cause: Exception)`.
std::string baseExcCtor() {
    return std::string(
        "module M; "
        "class C "
        "  private method DoWork() end DoWork; "
        "  public method Handle() "
        "    try DoWork(); "
        "    catch var ex: Exception => "
        "      throw new Exception(\"wrap\", ex); "        // <- arg pos=1 = `cause`
        "    end; "
        "  end Handle; "
        "end C;");
}

// T3 driver: a class with TWO exception-typed non-owned constructor params;
// the spec-marker `cause` is at position 0, the other (NOT cause-named) is
// at position 1. A `throw new W(b1, b2)` should be rejected on `b2` only —
// `b1` is in the cause slot and is the documented legal snapshot position.
std::string twoExcCtor(const std::string& lhsArg, const std::string& rhsArg) {
    return std::string(
        "module M; "
        "class W: Exception "
        "  private _first: Exception?; "
        "  private _second: Exception?; "
        "  public constructor(cause: Exception, other: Exception) "
        "    this._first := cause; "
        "    this._second := other; "
        "  end constructor; "
        "end W; "
        "class C "
        "  private method DoWork() end DoWork; "
        "  public method Handle() "
        "    try DoWork(); "
        "    catch var ex: Exception => do "
        "      var b := ex; "
        "      throw new W(" + lhsArg + ", " + rhsArg + "); "
        "    end; "
        "    end; "
        "  end Handle; "
        "end C;");
}

const char* kCaughtEscapesMessage = "caught exception `ex` may not be passed to a constructor";

} // namespace

// =====================================================================
// T1 (REGRESSION): `throw new W(ex)` where W is exception-derived but
// the argument lands in a NON-`cause`-named parameter of W's ctor.
// The §31.2.4 carve-out, narrowed to the cause slot, must NOT fire.
// REJECTED.  (§31.1 / #430, fix #3075.)
//
// Pre-fix: silently accepted — the §31.2.4 carve-out skipped the check
// at every exception-typed argument of an exception-derived ctor.
// Post-fix: the `e` parameter is not the cause slot, so the carve-out
// leaves the #430 check intact, and the bare catch-borrow identifier
// `ex` is rejected.
// =====================================================================
BEZEL_TEST(throw_cause_only_carveout_3075, throw_new_non_cause_param_exc_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrapAsExc() +
        "throw new Wrap(ex); "
        "    end; "
        "  end Handle; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kCaughtEscapesMessage));
}

// =====================================================================
// T2 (CONTROL): `throw new Exception("msg", ex)` uses the base
// `Exception` snapshot ctor — `constructor(message: string, cause: Exception)`
// (§31.4). The `ex` argument lands at position 1, the parameter named
// `cause`. The narrow carve-out fires for it; the #430 check is skipped.
// ACCEPTED.  (§31.2.4 / §31.4 — the spec's own wrap-and-rethrow example.)
//
// This is the legal idiom. Without this, every wrap-and-rethrow in a
// catch arm would be rejected. The narrow fix MUST preserve this.
// =====================================================================
BEZEL_TEST(throw_cause_only_carveout_3075, throw_new_base_cause_param_exc_clean_31_1) {
    auto r = parseAndCheckOwnership(baseExcCtor());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =====================================================================
// T3 (MIXED): `throw new W(b1, b2)` where W's ctor is
// `constructor(cause: Exception, other: Exception)`. `b1` is a catch
// borrow at the cause slot (skipped by the carve-out); `b2` is a
// re-borrow at the non-cause `other` slot (caught by #430). REJECTED,
// with the diagnostic on `b2` only — `b1` is the cause snapshot.
// =====================================================================
BEZEL_TEST(throw_cause_only_carveout_3075, throw_new_cause_then_non_cause_rejected_31_1) {
    // Use `ex` as `b1` (cause slot — borrow copied to snapshot, no
    // ownership storage) and `b` as `b2` (re-borrow holder for `ex`,
    // its `states_[b].source` is the catch borrow — but the ctor calls
    // out into the runtime's exception machinery in the same way for
    // either spelling; the trigger is the parameter NOT being `cause`).
    auto r = parseAndCheckOwnership(twoExcCtor("ex", "b"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kCaughtEscapesMessage));
}

// =====================================================================
// T3-CONTROL: the same class W with `b2` as an OWNED fresh exception
// (a `new Exception(...)` is not a borrow) — the second slot is
// non-owned but the argument is not a borrow, so #430 has nothing to
// report. ACCEPTED.  Ensures the narrow fix is not over-rejecting.
// =====================================================================
BEZEL_TEST(throw_cause_only_carveout_3075, throw_new_cause_then_owned_fresh_clean_31_1) {
    auto r = parseAndCheckOwnership(twoExcCtor("ex", "new Exception(\"inner\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
