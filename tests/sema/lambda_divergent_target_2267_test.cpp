// §27.3 / §5.1 (v0.236, issue #2267): a function-typed target alone does not
// put a block-bodied lambda's ReturnStmt visitors into a declared-result
// context. Divergent inferred return types must therefore still be diagnosed;
// otherwise incompatible returns reach codegen as malformed IR. A lambda's own
// valid return-type ascription remains the context that checks each return.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

bool hasDivergentReturnError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("divergent `return` types") != std::string::npos &&
            d.message.find("27.3") != std::string::npos)
            return true;
    return false;
}

bezel::test::OwnershipResult check(const std::string& body) {
    return parseAndCheckOwnership(
        "module M; class C public static method Main(): int " + body +
        " return 0; end Main; end C;");
}

}  // namespace

// §27.3 / §5.1 (#2267): an ascribed local function target does not itself check
// lambda ReturnStmt operands. Before the fix this passed sema and failed LLVM
// verification with `ret double` in an i64 function.
BEZEL_TEST(lambda_divergent_target_2267,
           ascribed_function_target_still_rejects_divergent_returns) {
    auto r = check(
        "var f: () -> long := lambda() => do "
        "  if true then return 1; end; "
        "  return 2.0; "
        "end; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDivergentReturnError(r));
}

// §27.3 (#2267): the same divergent body without any target remains rejected;
// this anchors the ordinary no-target inference rule beside the target bug.
BEZEL_TEST(lambda_divergent_target_2267,
           no_ascription_rejects_divergent_returns) {
    auto r = check(
        "var f := lambda() => do "
        "  if true then return 1; end; "
        "  return 2.0; "
        "end; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDivergentReturnError(r));
}

// §27.3 (#2267): a target whose branches infer the same return type is valid;
// tightening the suppression must not reject legitimately typed lambda bodies.
BEZEL_TEST(lambda_divergent_target_2267,
           matching_returns_under_target_remain_accepted) {
    auto r = check(
        "var f: () -> long := lambda() => do "
        "  if true then return 1; end; "
        "  return 2; "
        "end; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.3 / §5.1 (#2267): a lambda's own return ascription supplies the declared
// result context, so distinct operand types that both convert to long remain
// accepted and are not mistaken for targetless divergent inference.
BEZEL_TEST(lambda_divergent_target_2267,
           lambda_return_ascription_checks_and_accepts_convertible_returns) {
    auto r = check(
        "var f: () -> long := lambda(): long => do "
        "  if true then return 1; end; "
        "  return 2; "
        "end; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
