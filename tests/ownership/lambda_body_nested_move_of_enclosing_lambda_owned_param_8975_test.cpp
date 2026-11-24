// Ownership — a NESTED lambda may not consume (`move`) an `owned` PARAMETER of
// its ENCLOSING LAMBDA. The lambda form was admitted (rc=0, no diagnostic); the
// byte-identical METHOD twin (the same `move p`, `p` a local of the method
// body) is rejected. Issue #8975 (the `owned`-PARAMETER half of #8937's
// marked-transfer rule; the LOCAL half closed with #8937).
//
// SPEC: §28.3 "Captured owned bindings" (v0.359): "Inside a closure, a
// captured owned binding — an owned local or `owned` parameter of the
// enclosing body — is uniformly in borrow state: ... assignment to it, a
// marked transfer from it, an explicit `Dispose()` or discard of it, and every
// other owned-consuming use of it are compile errors". The "enclosing body" of
// a NESTED closure is the enclosing lambda's body, so the rule covers the
// lambda's `owned` parameter exactly as it covers its owned local.
//
// ROOT CAUSE: `MethodOwnership::reportEnclosingLambdaOwnedTransfer` resolved
// only a LocalSymbol owner (`lambdaOwnerOfDeclaringLambda`); a lambda's
// ParameterSymbol has no binding statement and no declaration location, so it
// answered "not owned". The owner is resolved through `ctx_.lambdaParams`
// (`lambdaOwnerOfParameter`, the route #8926 added for the `throw` form).
//
// MEASURED PRE-FIX: the program below compiled rc=0 with no diagnostic.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// The METHOD twin's exact diagnostic (the same wording as the LOCAL half).
const char* kCannotTransfer =
    "cannot transfer captured owned binding `p` inside a closure";

template <class Diags>
bool hasCannotTransfer(const Diags& ds) {
    for (const auto& d : ds.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(kCannotTransfer) != std::string::npos)
            return true;
    return false;
}

const char* kPrelude =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; "
    "end Res; "
    "class Sink "
    "  public static method Take(owned r: Res): int return r.tag; end Take; "
    "end Sink; ";

// The issue's repro: a lambda's `owned` parameter consumed by a NESTED lambda.
std::string nestedMoveOfOwnedParam() {
    return std::string(kPrelude) +
           "class C "
           "  public static method Main(): int "
           "    var f := lambda(owned p: Res): int => do "
           "      var g := lambda(): int => Sink.Take(move p); "
           "      return g(); "
           "    end; "
           "    Console.WriteLine(f(new Res(1))); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

BEZEL_TEST(lambda_body_nested_move_of_enclosing_lambda_owned_param_8975,
           nested_lambda_move_of_enclosing_owned_param_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(nestedMoveOfOwnedParam());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasCannotTransfer(r.diagnostics));
}

BEZEL_TEST(lambda_body_nested_move_of_enclosing_lambda_owned_param_8975,
           aot_nested_lambda_move_of_enclosing_owned_param_is_a_compile_error_28_3) {
    JitResult jr = runAot(nestedMoveOfOwnedParam());
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(hasCannotTransfer(jr.diagnostics));
}
