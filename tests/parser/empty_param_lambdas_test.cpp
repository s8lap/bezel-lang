// Spec-derived parser tests for the empty-params lambda subset of A.7
// LambdaExpr. Parameterised lambdas (`(x) => …`, `(x: T) => …`,
// `async (...) => …`) need multi-token lookahead and are deferred.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

static std::string parseInitText(const std::string& expr) {
    auto r = parseAll(std::string("module M; class Foo public x: int := ") +
                      expr + std::string("; end Foo;"));
    if (r.diagnostics.hasErrors()) return "<error>";
    const auto ms = memberSummaries(r);
    if (ms.size() != 1) return "<no member>";
    const std::string prefix = "public x: int := ";
    if (ms[0].rfind(prefix, 0) != 0) return "<bad prefix>";
    return ms[0].substr(prefix.size());
}

BEZEL_TEST(parser_lambda, empty_params_expression_body) {
    BEZEL_EXPECT_EQ(parseInitText("() => 42"),
                    std::string("(() => 42)"));
}

BEZEL_TEST(parser_lambda, empty_params_call_body) {
    BEZEL_EXPECT_EQ(parseInitText("() => f(x)"),
                    std::string("(() => f(x))"));
}

BEZEL_TEST(parser_lambda, empty_params_block_body) {
    BEZEL_EXPECT_EQ(
        parseInitText("() => do x := 1; y := 2; end"),
        std::string("(() => do { x := 1; y := 2; } end)"));
}

BEZEL_TEST(parser_lambda, empty_params_empty_block_body) {
    BEZEL_EXPECT_EQ(parseInitText("() => do end"),
                    std::string("(() => do {  } end)"));
}

// Lambda as argument to a call — common pattern.
BEZEL_TEST(parser_lambda, lambda_as_call_argument) {
    BEZEL_EXPECT_EQ(parseInitText("subscribe(() => handle())"),
                    std::string("subscribe((() => handle()))"));
}
