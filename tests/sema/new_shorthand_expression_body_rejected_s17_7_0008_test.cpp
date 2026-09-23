// Spec-derived regression test for rule-corpus `s17_7_0008` (spec §17.7,
// v0.307).
//
// THE GOVERNING RULE. Spec §17.7 "The `new` operator" (v0.307): "The
// shorthand is permitted at the written-declared-type positions (i), (ii),
// (iii) and the `return e;` statement of (v) above: field initializers,
// typed local-variable declarations, default parameter values, and return
// statements (when the method has a declared return type). It is **not**
// permitted at an expression body or at a lambda's ascribed return."
//
// ROOT CAUSE. `runMethodBody`'s `exprBody` branch (src/sema/ExprTypecheck.cpp)
// — which handles every expression-bodied member (method, operator,
// property getter, accessor) — called `visitExpr(*exprBody)` with no gate at
// all for a typeless (shorthand) `NewExpr`. Since the shorthand's
// target-type wiring is granted only at the four positions above (never at
// an expression body), the untyped NewExpr fell straight through to
// codegen's #2491 zero/null degraded-lowering backstop instead of being
// diagnosed — the SAME defect class #4249 had already closed for a lambda's
// ascribed return (`rejectNewShorthandAtLambdaReturn`), left standing at
// this sibling position.
//
// MEASURED on `bf7a7515` (this lane's base, `r19-c20`) with `build/bezelc`:
// `public static method Make(): owned Config => new();` compiled with rc=0
// and printed a degraded-lowering `internal:` note ("return-value expression
// failed to lower to a value (degraded to a zero/null return value) —
// #2491") instead of a proper diagnostic.
//
// THE FIX. `rejectNewShorthandAtExpressionBody`, a new sibling of
// `rejectNewShorthandAtLambdaReturn`, called from `runMethodBody`'s
// `exprBody` branch before the body visit.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool rejectsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool runs(int expected, const std::string& src) {
    auto r = runAot(src);
    return r.ran && r.exitCode == expected;
}

// rule-corpus `s17_7_0008`, verbatim shape: an expression-bodied static
// method whose body is the bare `new()` shorthand.
const char* kNewShorthandAtExpressionBody =
    "module Conf; "
    "public class Config "
    "  public constructor() "
    "  end constructor; "
    "end Config; "
    "public class Factory "
    "  public static method Make(): owned Config => new(); "
    "end Factory;";

}  // namespace

BEZEL_TEST(sema_new_shorthand_expression_body_rejected_s17_7_0008,
           expression_bodied_method_new_shorthand_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(kNewShorthandAtExpressionBody,
                                   "not permitted at an expression body"));
    BEZEL_EXPECT_TRUE(
        rejectsWith(kNewShorthandAtExpressionBody, "\xC2\xA7""17.7"));
}

// Companion: a getter-form expression-bodied PROPERTY hits the same
// `runMethodBody` `exprBody` branch and must be rejected identically.
BEZEL_TEST(sema_new_shorthand_expression_body_rejected_s17_7_0008,
           expression_bodied_property_new_shorthand_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module Conf; "
        "public class Config "
        "  public constructor() "
        "  end constructor; "
        "end Config; "
        "public class Factory "
        "  public property Fresh: Config => new(); "
        "end Factory;",
        "not permitted at an expression body"));
}

// ACCEPT control: the explicit-name spelling at the identical expression-body
// position is entirely unaffected (only the SHORTHAND — a typeless NewExpr —
// is excluded from this position; naming the type explicitly is always
// legal).
BEZEL_TEST(sema_new_shorthand_expression_body_rejected_s17_7_0008,
           explicit_new_at_expression_body_still_accepted) {
    BEZEL_EXPECT_TRUE(runs(1,
        "module Conf; "
        "public class Config "
        "  public constructor() "
        "  end constructor; "
        "end Config; "
        "public class Factory "
        "  public static method Make(): owned Config => new Config(); "
        "end Factory; "
        "public class Program "
        "  public static method Main(): int "
        "    var c := Factory.Make(); "
        "    _ := c; "
        "    return 1; "
        "  end Main; "
        "end Program;"));
}

// ACCEPT control: the shorthand at a GRANTED written-declared-type position
// — a `return e;` statement of a block-bodied method with a declared return
// type — must be unaffected.
BEZEL_TEST(sema_new_shorthand_expression_body_rejected_s17_7_0008,
           new_shorthand_at_return_statement_still_accepted) {
    BEZEL_EXPECT_TRUE(runs(1,
        "module Conf; "
        "public class Config "
        "  public constructor() "
        "  end constructor; "
        "end Config; "
        "public class Factory "
        "  public static method Make(): owned Config "
        "    return new(); "
        "  end Make; "
        "end Factory; "
        "public class Program "
        "  public static method Main(): int "
        "    var c := Factory.Make(); "
        "    _ := c; "
        "    return 1; "
        "  end Main; "
        "end Program;"));
}
