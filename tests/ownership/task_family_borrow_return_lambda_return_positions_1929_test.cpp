// Spec-derived regression tests for #1929 (§28.3 "Task-family calls produce
// owned frames", v0.190, SB-01, sub-case SB01-B) — the callee-side
// borrow-return prohibition on a LAMBDA / function-value body, applied to the
// RETURN positions #1919/#1926 left unreached.
//
// §28.3 (v0.190): "**Callee side:** an unmarked `Task`-family return type is
// itself ownership-transferring for the task container ... so every return path
// of a non-`async` method **or lambda** with such a return type must supply an
// owned task ... Returning a **borrow** — a bare task field or property read, a
// plain parameter, a captured binding — is a compile error."
//
// #1919 wired the lambda-body enforcement into method BLOCK bodies (run()) and
// method `=>` EXPRESSION bodies (runExprBodyOwningReturnCheck); #1926 wired it
// into field initializers, property accessors, and expression-bodied operators.
// But the SB01-B classifier keys on the lambda's inferred function
// `TypeRef::funcReturn`, which the type-checker populates ONLY when an expected
// function type is threaded into inference — the typed-local #829 hint path.
// In a RETURN position no such expected type is threaded, so the inferred
// funcReturn is null and the check was INERT for:
//   * an expression-bodied METHOD whose body IS the lambda
//     (`method M(): Fn => (t) => t;`) — #1926 wired this for expr-bodied
//     OPERATORS but not methods;
//   * a lambda returned DIRECTLY from a block method body
//     (`method M(): Fn return (t) => t; end`);
//   * a lambda returned DIRECTLY from a BLOCK getter accessor
//     (`property P: Fn get => do return (t) => t; end`).
// An identity `Task`-family lambda `(t) => t` in any of these compiled CLEAN and
// would HANG at runtime exactly as the #1919 method/local form did.
//
// #1929 threads the enclosing DECLARED return function type (the method's
// return type, a block getter's property type) into the block/return drivers as
// the effective-return fallback for a lambda returned DIRECTLY, and passes the
// method's return type as the top expected type for an expression-bodied method
// — the same declared-position fallback #1926 used, now reaching return
// position. Only return-direct lambdas get the fallback; a lambda in any other
// (e.g. local-decl) position keeps nullptr, so the threading is surgical.
//
// Fails-without / passes-with: before the fix each REJECT program below compiled
// with no SB01-B diagnostic (verified by A/B build); this file asserts the
// SB01-B lambda message is present. The ADMIT set (a forwarded task-returning
// call, an `async` body, a completed-task factory, an `owned` parameter, any
// NON-`Task` return, and a non-return-direct local lambda) must not be
// over-rejected. A valid fresh-task returned-lambda then compiles, links, runs.

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
// the lambda(s) under test. The lambda's effective return type — read from the
// enclosing DECLARED return type in these positions — is what SB01-B keys on.
std::string wrapMember(const std::string& member) {
    return std::string(
        "module M; import System.Threading; "
        "class C ") + member +
        "  public static method Main(): int return 0; end Main; "
        "end C;";
}

} // namespace

// ---- REJECT: an identity `Task`-family lambda `(t) => t` returned at each of
//      the three #1929 return positions is rejected with the SB01-B message. ---

// EXPRESSION-bodied METHOD whose body IS the lambda
// (`method M(): (Task<int>) -> Task<int> => (t) => t;`). #1926 wired the
// declared-type fallback for expr OPERATORS but not expr METHODS.
BEZEL_TEST(task_family_borrow_return_lambda_return_positions_1929,
           expr_bodied_method_identity_lambda_rejected) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public static method Make(): (Task<int>) -> Task<int> => (t) => t; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasLambdaSb01Reject(r));
}

// BLOCK method body: a lambda returned DIRECTLY
// (`method M(): (Task<int>) -> Task<int> return (t) => t; end`).
BEZEL_TEST(task_family_borrow_return_lambda_return_positions_1929,
           block_method_return_identity_lambda_rejected) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public static method Make(): (Task<int>) -> Task<int> "
        "  return (t) => t; end Make; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasLambdaSb01Reject(r));
}

// BLOCK getter accessor: a lambda returned DIRECTLY
// (`property Echo: (Task<int>) -> Task<int> get => do return (t) => t; end`).
BEZEL_TEST(task_family_borrow_return_lambda_return_positions_1929,
           block_getter_return_identity_lambda_rejected) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public property Echo: (Task<int>) -> Task<int> "
        "  get => do return (t) => t; end; end Echo; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasLambdaSb01Reject(r));
}

// BLOCK method body: a return-direct lambda nested inside control flow (`if`)
// is reached — the return traversal descends into branch bodies.
BEZEL_TEST(task_family_borrow_return_lambda_return_positions_1929,
           block_method_return_under_if_identity_lambda_rejected) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public static method Make(flag: bool): (Task<int>) -> Task<int> "
        "  if flag then return (t) => t; end "
        "  return (u) => u; end Make; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasLambdaSb01Reject(r));
}

// ---- ACCEPT: the ADMIT set the check must NOT reject at the return positions
//      (no over-rejection). Each asserts the SB01-B lambda message is absent. ---

// ADMIT (expr method): forward a fresh task-returning CALL.
BEZEL_TEST(task_family_borrow_return_lambda_return_positions_1929,
           expr_method_forward_task_call_accepted) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public static async method Producer(): Task<int> return 5; "
        "  end Producer; "
        "public static method Make(): () -> Task<int> => () => Producer(); "));
    BEZEL_EXPECT_FALSE(hasLambdaSb01Reject(r));
}

// ADMIT (block method): forward a fresh task-returning CALL from a `return`.
BEZEL_TEST(task_family_borrow_return_lambda_return_positions_1929,
           block_method_forward_task_call_accepted) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public static async method Producer(): Task<int> return 5; "
        "  end Producer; "
        "public static method Make(): () -> Task<int> "
        "  return () => Producer(); end Make; "));
    BEZEL_EXPECT_FALSE(hasLambdaSb01Reject(r));
}

// ADMIT (block getter): forward a fresh task-returning CALL from a `return`.
BEZEL_TEST(task_family_borrow_return_lambda_return_positions_1929,
           block_getter_forward_task_call_accepted) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public static async method Producer(): Task<int> return 5; "
        "  end Producer; "
        "public property Echo: () -> Task<int> "
        "  get => do return () => Producer(); end; end Echo; "));
    BEZEL_EXPECT_FALSE(hasLambdaSb01Reject(r));
}

// ADMIT (block method): an `async` returned lambda — its body is the awaited
// value, not a task.
BEZEL_TEST(task_family_borrow_return_lambda_return_positions_1929,
           block_method_return_async_lambda_accepted) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public static async method Producer(): Task<int> return 5; "
        "  end Producer; "
        "public static method Make(): () -> Task<int> "
        "  return async () => await Producer(); end Make; "));
    BEZEL_EXPECT_FALSE(hasLambdaSb01Reject(r));
}

// ADMIT (block method): a completed-task factory (§30.10).
BEZEL_TEST(task_family_borrow_return_lambda_return_positions_1929,
           block_method_return_completed_task_factory_accepted) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public static method Make(): () -> Task<int> "
        "  return () => Task.FromResult<int>(5); end Make; "));
    BEZEL_EXPECT_FALSE(hasLambdaSb01Reject(r));
}

// ADMIT (expr method): an `owned` lambda parameter returned through a `owned`
// `Task` return — the transfer contract, not this rule's plain-`Task` case.
BEZEL_TEST(task_family_borrow_return_lambda_return_positions_1929,
           expr_method_owned_param_owned_return_accepted) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public static method Make(): (owned Task<int>) -> owned Task<int> "
        "  => (owned t) => t; "));
    BEZEL_EXPECT_FALSE(hasLambdaSb01Reject(r));
}

// UNAFFECTED (block method): a returned lambda of a NON-`Task` type — a
// value-typed identity `(x) => x` — is not this rule's case and is untouched.
BEZEL_TEST(task_family_borrow_return_lambda_return_positions_1929,
           block_method_return_non_task_identity_unaffected) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public static method Make(): (int) -> int "
        "  return (x) => x; end Make; "));
    BEZEL_EXPECT_FALSE(hasLambdaSb01Reject(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// SURGICAL (block method): the enclosing method's declared `Task`-family
// function return type must reach ONLY the return-direct lambda — a
// non-violating local lambda in the same body must NOT inherit it (else it
// would be over-rejected). The valid factory return is accepted.
BEZEL_TEST(task_family_borrow_return_lambda_return_positions_1929,
           non_return_direct_local_lambda_not_over_rejected) {
    auto r = parseAndCheckOwnership(wrapMember(
        "public static method Make(): () -> Task<int> "
        "  var g := (x: int) => x; "
        "  var h: int := g(3); "
        "  return () => Task.FromResult<int>(5); end Make; "));
    BEZEL_EXPECT_FALSE(hasLambdaSb01Reject(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- BEHAVIOR: a VALID fresh-task returned lambda compiles, links, and runs
//      (the runtime the pre-fix identity lambda would have corrupted). ----------

BEZEL_TEST(task_family_borrow_return_lambda_return_positions_1929,
           fresh_task_returned_lambda_runs) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Producer(): Task<int> return 7; "
        "  end Producer; "
        "  public static method MakeFactory(): () -> Task<int> "
        "    return () => Producer(); end MakeFactory; "
        "  public static async method Main(): Task<int> "
        "    var mk := C.MakeFactory(); "
        "    using var t := mk(); "
        "    var x := await t; "
        "    return x; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
