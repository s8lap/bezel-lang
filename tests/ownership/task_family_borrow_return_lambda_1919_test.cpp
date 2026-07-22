// Spec-derived regression tests for #1919 (§28.3 "Task-family calls produce
// owned frames", v0.190, SB-01, sub-case SB01-B) — the callee-side
// borrow-return prohibition on LAMBDA / function-value bodies.
//
// §28.3 (v0.190): "**Callee side:** an unmarked `Task`-family return type is
// itself ownership-transferring for the task container ... so every return path
// of a non-`async` method **or lambda** with such a return type must supply an
// owned task ... Returning a **borrow** — a bare task field or property read, a
// plain parameter, a captured binding — is a compile error."
//
// The SB01-B enforcement (checkBorrowReturnedThroughOwningType) was wired into
// method/static body checking (handleReturn / runExprBodyOwningReturnCheck) but
// NOT into lambda / function-value bodies, which are not otherwise
// ownership-analysed. So an identity lambda that returns a borrowed `Task`
// parameter through a plain-`Task` return COMPILED CLEAN and then HUNG at
// runtime (double-free / async-state corruption), while the equivalent method
// `Echo(t: Task<int>): Task<int> return t;` was correctly rejected. This file
// pins the lambda-body enforcement: the identity lambda is now rejected with
// the same SB01-B diagnostic; the ADMIT set (a forwarded task-returning call,
// an `async` body, a completed-task factory, an `owned` parameter, and any
// NON-`Task` return) is not over-rejected.
//
// Fails-without / passes-with: before the fix the two REJECT programs below
// compiled with no diagnostic (verified by stash+rebuild A/B); this file's
// reject cases assert `hasErrors()` and the SB01-B message, so they fail against
// the pre-fix compiler. The behavior test then confirms a VALID fresh-task
// lambda still compiles, links, and runs.

#include <string>

#include "support/aot_support.hpp"       // runAot — real AOT path
#include "support/ownership_support.hpp" // parseAndCheckOwnership — front-end oracle
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// True when the ownership pass emitted the SB01-B lambda-return rejection
// (distinct wording from the named-method form: "... return of a lambda").
bool hasLambdaSb01Reject(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("`Task`-family return of a lambda") !=
            std::string::npos)
            return true;
    }
    return false;
}

// Wrap a Main body that only DECLARES the lambda(s) under test (never calls
// them) — enough to drive the callee-side return check without runtime
// concerns. Main is a plain (non-async) `int` method; the lambda's own declared
// return type — not the enclosing method's — is what SB01-B keys on.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; import System.Threading; "
        "class C "
        "  public static method Main(): int "
        "    ") + body + " return 0; "
        "  end Main; "
        "end C;";
}

} // namespace

// ---- REJECT: a borrowed `Task` parameter returned through a plain-`Task`
//      lambda return, matching the rejected method form `Echo`. ----------------

// The issue's exact repro: an expression-bodied identity lambda `(t) => t`.
BEZEL_TEST(task_family_borrow_return_lambda_1919,
           identity_expr_body_lambda_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "var echo: (Task<int>) -> Task<int> := lambda(t) => t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasLambdaSb01Reject(r));
}

// The block-body sibling `(t) => do return t; end` is rejected the same way —
// parity with the method form's block body.
BEZEL_TEST(task_family_borrow_return_lambda_1919,
           identity_block_body_lambda_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "var echo: (Task<int>) -> Task<int> := lambda(t) => do return t; end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasLambdaSb01Reject(r));
}

// A NON-`owned` `Task`-family FIELD read captured and returned from a lambda is
// a borrow of its storage — rejected exactly as the method form's `GetTask`.
BEZEL_TEST(task_family_borrow_return_lambda_1919,
           captured_task_field_read_lambda_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Threading; "
        "class Cache "
        "  private _cachedTask: Task<int>; "
        "  public constructor(t: Task<int>) _cachedTask := t; end constructor; "
        "  public method Make(): () -> Task<int> "
        "    var f: () -> Task<int> := lambda() => _cachedTask; "
        "    return f; "
        "  end Make; "
        "end Cache; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasLambdaSb01Reject(r));
}

// ---- ACCEPT: the ADMIT set the lambda check must NOT reject (no
//      over-rejection). Each asserts the SB01-B lambda message is absent. ------

// ADMIT: forward a fresh task-returning CALL (SB01-A owned result) — the
// dominant `() => Producer()` shape.
BEZEL_TEST(task_family_borrow_return_lambda_1919,
           forward_task_call_lambda_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Producer(): Task<int> return 5; "
        "  end Producer; "
        "  public static method Main(): int "
        "    var f: () -> Task<int> := lambda() => Producer(); return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasLambdaSb01Reject(r));
}

// ADMIT: an `async` lambda — its body is the awaited value (an int), not a
// task; the async state machine is its own fresh frame and must never be
// classified by SB01-B.
BEZEL_TEST(task_family_borrow_return_lambda_1919,
           async_lambda_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Producer(): Task<int> return 5; "
        "  end Producer; "
        "  public static method Main(): int "
        "    var f: () -> Task<int> := async lambda() => await Producer(); return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasLambdaSb01Reject(r));
}

// ADMIT: a completed-task factory (§30.10).
BEZEL_TEST(task_family_borrow_return_lambda_1919,
           completed_task_factory_lambda_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "var f: () -> Task<int> := lambda() => Task.FromResult<int>(5);"));
    BEZEL_EXPECT_FALSE(hasLambdaSb01Reject(r));
}

// ADMIT: an `owned` lambda parameter returned through a plain-`Task` return —
// the spec's "an `owned` task local or parameter" source.
BEZEL_TEST(task_family_borrow_return_lambda_1919,
           owned_param_lambda_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "var echo: (owned Task<int>) -> Task<int> := lambda(owned t) => t;"));
    BEZEL_EXPECT_FALSE(hasLambdaSb01Reject(r));
}

// UNAFFECTED: a lambda returning a borrow of a NON-`Task` type — value-typed
// identity `(x) => x` — is not this rule's case and must not be touched.
BEZEL_TEST(task_family_borrow_return_lambda_1919,
           non_task_identity_lambda_unaffected) {
    auto r = parseAndCheckOwnership(wrap(
        "var id: (int) -> int := lambda(x) => x;"));
    BEZEL_EXPECT_FALSE(hasLambdaSb01Reject(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- BEHAVIOR: a VALID fresh-task lambda still compiles, links, and runs
//      (the runtime the pre-fix identity lambda corrupted). --------------------

BEZEL_TEST(task_family_borrow_return_lambda_1919,
           fresh_task_lambda_runs) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Producer(): Task<int> return 9; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var f: () -> Task<int> := lambda() => Producer(); "
        "    var t := f(); "
        "    var x := await t; "
        "    return x; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}
