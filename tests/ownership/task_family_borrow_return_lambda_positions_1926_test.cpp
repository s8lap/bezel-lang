// Spec-derived regression tests for #1926 (§28.3 "Task-family calls produce
// owned frames", v0.190, SB-01, sub-case SB01-B) — the callee-side
// borrow-return prohibition on a LAMBDA / function-value body, applied to the
// member positions #1919 left unreached.
//
// §28.3 (v0.190): "**Callee side:** an unmarked `Task`-family return type is
// itself ownership-transferring for the task container ... so every return path
// of a non-`async` method **or lambda** with such a return type must supply an
// owned task ... Returning a **borrow** — a bare task field or property read, a
// plain parameter, a captured binding — is a compile error."
//
// #1919 (checkLambdaTaskFamilyBorrowReturns) wired the lambda-body enforcement
// into method BLOCK bodies (run()) and method `=>` EXPRESSION bodies
// (runExprBodyOwningReturnCheck) — i.e. a lambda ASSIGNED TO A TYPED LOCAL was
// rejected. But the pre-pass never reached three other positions where a lambda
// can appear: a FIELD INITIALIZER, a property/indexer GETTER body, and an
// EXPRESSION-bodied OPERATOR. In those positions an identity `Task`-family
// lambda `(t) => t` compiled clean and would HANG at runtime exactly as the
// #1919 method/local form did (benign under-rejection — no position was
// ownership-analysed for this before #1919 either).
//
// #1926 wires the same pre-pass into checkMember's field-initializer, property
// accessor (get AND set), and expression-bodied-operator arms. Because the
// type-checker leaves a lambda's inferred `funcReturn` un-inferred off the
// typed-local path (it is null for a lambda in these positions), the pre-pass
// additionally takes the expected function type at the position (the field /
// property / operator declared type) as the effective-return fallback for the
// top-level lambda.
//
// Fails-without / passes-with: before the fix each REJECT program below compiled
// with no SB01-B diagnostic (verified by A/B build); this file asserts the
// SB01-B lambda message is present. The ADMIT set (a forwarded task-returning
// call, an `async` body, a completed-task factory, an `owned` parameter, and any
// NON-`Task` return) must not be over-rejected. A valid fresh-task field lambda
// then compiles, links, and runs.

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

// A class C carrying `member` plus a trivial non-async Main; the member declares
// the lambda(s) under test. The lambda's OWN declared return type — not Main's —
// is what SB01-B keys on.
std::string wrapMember(const std::string& member) {
    return std::string(
        "module M; import System.Threading; "
        "class C ") + member +
        "  public static method Main(): int return 0; end Main; "
        "end C;";
}

} // namespace

// ---- REJECT: an identity `Task`-family lambda `(t) => t` at each of the three
//      #1926 positions (plus a setter body) is rejected with the SB01-B message.

// FIELD INITIALIZER (`f: (Task<int>) -> Task<int> := (t) => t;`).
BEZEL_TEST(task_family_borrow_return_lambda_positions_1926,
           field_initializer_identity_lambda_rejected) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public echo: (Task<int>) -> Task<int> := lambda(t) => t; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasLambdaSb01Reject(r));
}

// EXPRESSION-bodied property GETTER (`property Echo: Fn => (t) => t;`).
BEZEL_TEST(task_family_borrow_return_lambda_positions_1926,
           getter_expr_body_identity_lambda_rejected) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public property Echo: (Task<int>) -> Task<int> => lambda(t) => t; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasLambdaSb01Reject(r));
}

// EXPRESSION-bodied OPERATOR (`operator +(...): Fn => (t) => t;`).
BEZEL_TEST(task_family_borrow_return_lambda_positions_1926,
           expr_bodied_operator_identity_lambda_rejected) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public static method operator +(a: C, b: C): "
        "(Task<int>) -> Task<int> => lambda(t) => t; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasLambdaSb01Reject(r));
}

// PROPERTY SETTER body (a typed-local identity lambda): a lambda's SB01-B
// obligation is independent of the accessor role, so a setter body — never
// ownership-analysed for lambdas before #1926 — is covered too.
BEZEL_TEST(task_family_borrow_return_lambda_positions_1926,
           setter_body_identity_lambda_rejected) {
    auto r = parseAndCheckOwnership(wrapMember(
        "private _v: int := 0; "
        "public property P: int "
        "  get => _v; "
        "  set => do var echo: (Task<int>) -> Task<int> := lambda(t) => t; "
        "             _v := value; end; "
        "end P; "));
    BEZEL_EXPECT_TRUE(hasLambdaSb01Reject(r));
}

// ---- ACCEPT: the ADMIT set the check must NOT reject at the new positions
//      (no over-rejection). Each asserts the SB01-B lambda message is absent. ---

// ADMIT (field): forward a fresh task-returning CALL (`() => Producer()`).
BEZEL_TEST(task_family_borrow_return_lambda_positions_1926,
           field_forward_task_call_accepted) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public static async method Producer(): Task<int> return 5; "
        "  end Producer; "
        "public f: () -> Task<int> := lambda() => Producer(); "));
    BEZEL_EXPECT_FALSE(hasLambdaSb01Reject(r));
}

// ADMIT (field): an `async` lambda — its body is the awaited value, not a task.
BEZEL_TEST(task_family_borrow_return_lambda_positions_1926,
           field_async_lambda_accepted) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public static async method Producer(): Task<int> return 5; "
        "  end Producer; "
        "public f: () -> Task<int> := async lambda() => await Producer(); "));
    BEZEL_EXPECT_FALSE(hasLambdaSb01Reject(r));
}

// ADMIT (field): a completed-task factory (§30.10).
BEZEL_TEST(task_family_borrow_return_lambda_positions_1926,
           field_completed_task_factory_accepted) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public f: () -> Task<int> := lambda() => Task.FromResult<int>(5); "));
    BEZEL_EXPECT_FALSE(hasLambdaSb01Reject(r));
}

// ADMIT (operator): an `owned` lambda parameter returned through a plain-`Task`
// return — the spec's "an `owned` task local or parameter" source.
BEZEL_TEST(task_family_borrow_return_lambda_positions_1926,
           operator_owned_param_accepted) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public static method operator +(a: C, b: C): "
        "(owned Task<int>) -> Task<int> => lambda(owned t) => t; "));
    BEZEL_EXPECT_FALSE(hasLambdaSb01Reject(r));
}

// ADMIT (getter): forward a fresh task-returning CALL from a getter body.
BEZEL_TEST(task_family_borrow_return_lambda_positions_1926,
           getter_forward_task_call_accepted) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public static async method Producer(): Task<int> return 5; "
        "  end Producer; "
        "public property Echo: () -> Task<int> => lambda() => Producer(); "));
    BEZEL_EXPECT_FALSE(hasLambdaSb01Reject(r));
}

// UNAFFECTED (field): a lambda returning a borrow of a NON-`Task` type — a
// value-typed identity `(x) => x` — is not this rule's case and is untouched.
BEZEL_TEST(task_family_borrow_return_lambda_positions_1926,
           field_non_task_identity_lambda_unaffected) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public id: (int) -> int := lambda(x) => x; "));
    BEZEL_EXPECT_FALSE(hasLambdaSb01Reject(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- BEHAVIOR: a VALID fresh-task field lambda compiles, links, and runs
//      (the runtime the pre-fix identity lambda would have corrupted). ----------

BEZEL_TEST(task_family_borrow_return_lambda_positions_1926,
           fresh_task_field_lambda_runs) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class C "
        "  public static f: () -> Task<int> := lambda() => Producer(); "
        "  public static async method Producer(): Task<int> return 7; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    using var t := C.f(); "
        "    var x := await t; "
        "    return x; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
