// Spec-derived tests for §29.11 (v0.334) *Multiple enumeration*, the
// must-use suppression at a statically visible consumption point.
//
// §29.11 makes a lazy query's ENUMERATION RIGHT single-consumption and layers
// it on top of the ordinary §28.3 container obligation:
//
//   "a bound lazy query whose enumeration right is never spent draws K3.14's
//    must-use warning at its auto-drop, naming the binding ... and that
//    warning is suppressed where the binding's enumeration right is spent at
//    a statically visible consumption point — the normative list below — the
//    auto-drop still running."
//
// The normative list is CLOSED, and every item is spelled here:
//
//   1. Enumeration or a terminal operator (`for`, `Count`, `First`, `ToArray`,
//      ...)
//   2. Applying a further deferred operator: `var q2 := q.Select(f);`
//   3. Passing the query as an `IEnumerable<T>` argument.
//
// The classification that arms the lint lives in producerState's §29.11 arm
// (#c1e6187a): a deferred-activation call result is Owned, so an undischarged
// plain-`var` binding draws K3.14's warning at its auto-drop. That arm is what
// makes row `s29_11_0031` (no consumption point) warn. The SUPPRESSION half
// was the missing half: nothing consulted the consumption points, so a query
// that IS enumerated — row `s29_11_0032`, `for var x in q` — warned anyway.
//
// Each test below is one item of the normative list, plus the mirror control
// that must KEEP warning. The controls are load-bearing: they are what proves
// the fix suppressed the lint rather than disabling it, and each asserts the
// warning by NAME (`q`), so an unrelated owned local drawing it cannot stand
// in for the rule.
//
// NOT A DISPOSAL CHANGE. The suppression removes only the diagnostic. The
// `autoDropAtScopeEnd` flag is still set at both sites (the sync guard wraps
// the report, the async guard returns before it), so codegen's scope-end
// disposal is bit-for-bit unchanged — which is what §29.11's "the auto-drop
// still running" and "A spent right is §28.3's one suppression condition that
// discharges no obligation" require.
//
// Every quiet case also asserts `!hasErrors()`: a program that failed to parse
// or typecheck would emit no warning for the trivial reason that the binding
// was never analysed, which would make the quiet assertion vacuous.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::parseAndCheckOwnership;

namespace {

// Wrap a method body in the standard single-class compilation unit. `n` is the
// scrutinee the `case` tests match on; it is a value-typed parameter, so it
// carries no §28.3 obligation and the cases that never read it are unaffected.
std::string prog(const std::string& body) {
    return "module M; public class Scanner "
           "  public method Scan(xs: Array<int>, n: int): int\n" +
           body +
           "  end Scan;\n"
           "end Scanner;\n";
}

}  // namespace

// §29.11 item 1, ENUMERATION. The shape of conformance row `s29_11_0032`
// verbatim: the `for` over `q` IS the statically visible consumption point, so
// the must-use warning is suppressed while the auto-drop still runs.
BEZEL_TEST(ownership_29_11, for_in_enumeration_suppresses_must_use) {
    auto r = parseAndCheckOwnership(prog(
        "    var q := xs.Where(lambda(x) => x > 0);\n"
        "    for var x in q do\n"
        "      Console.WriteLine($\"{x}\");\n"
        "    end for;\n"
        "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "q"));
}

// The MIRROR of the case above, and the control the fix must not weaken:
// conformance row `s29_11_0031` binds the identical query and never spends its
// enumeration right, so the lint MUST fire — naming `q`.
BEZEL_TEST(ownership_29_11, no_consumption_point_still_draws_must_use) {
    auto r = parseAndCheckOwnership(prog(
        "    var q := xs.Where(lambda(x) => x > 0);\n"
        "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "q"));
}

// §29.11 item 1, TERMINAL OPERATOR. `Count()` spends the right exactly as the
// `for` does; only the spelling differs.
BEZEL_TEST(ownership_29_11, terminal_operator_suppresses_must_use) {
    auto r = parseAndCheckOwnership(prog(
        "    var q := xs.Where(lambda(x) => x > 0);\n"
        "    return q.Count();\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "q"));
}

// §29.11 item 2. `var q2 := q.Select(f);` spends `q`'s right — the obligation
// transfers into the composed query while the container obligation stays put —
// so NEITHER binding may draw the lint once `q2` is itself enumerated.
BEZEL_TEST(ownership_29_11, further_deferred_operator_suppresses_must_use) {
    auto r = parseAndCheckOwnership(prog(
        "    var q := xs.Where(lambda(x) => x > 0);\n"
        "    var q2 := q.Select(lambda(x) => x + 1);\n"
        "    return q2.Count();\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "q"));
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "q2"));
}

// §29.11 item 3. The plain-argument transfer: the right moves to the callee and
// the frame's DISPOSAL obligation stays with the caller (the argument is an
// ordinary borrow), so `q` is silent here.
BEZEL_TEST(ownership_29_11, ienumerable_argument_suppresses_must_use) {
    // Written whole rather than through `prog()`: `Consume` is a SIBLING of
    // `Scan`, so the second method has to sit in the class, not in the body.
    auto r = parseAndCheckOwnership(
        "module M; public class Scanner\n"
        "  public method Scan(xs: Array<int>): int\n"
        "    var q := xs.Where(lambda(x) => x > 0);\n"
        "    return Consume(q);\n"
        "  end Scan;\n"
        "\n"
        "  public static method Consume(src: IEnumerable<int>): int\n"
        "    var n := 0;\n"
        "    for var e in src do n := n + 1; end for;\n"
        "    return n;\n"
        "  end Consume;\n"
        "end Scanner;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "q"));
}

// The item-3 control: the SAME program with the transfer removed. `q` is bound
// and never spends its right, so the lint must fire — the fix suppresses on
// the transfer, not on the mere presence of an `IEnumerable<int>` in the file.
BEZEL_TEST(ownership_29_11, untransferred_query_still_draws_must_use) {
    auto r = parseAndCheckOwnership(
        "module M; public class Scanner\n"
        "  public method Scan(xs: Array<int>): int\n"
        "    var q := xs.Where(lambda(x) => x > 0);\n"
        "    return 0;\n"
        "  end Scan;\n"
        "\n"
        "  public static method Consume(src: IEnumerable<int>): int\n"
        "    var n := 0;\n"
        "    for var e in src do n := n + 1; end for;\n"
        "    return n;\n"
        "  end Consume;\n"
        "end Scanner;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "q"));
}

// §29.11 does not confine the consumption point to the straight-line body: a
// `case` arm is as statically visible as the code around it, so a `for` under
// a `var y => do … end` arm spends the right exactly as the same `for` outside
// the match does. This is the reachability witness for the CaseStmt arm —
// without it the arm is untested and the lint false-positives here.
BEZEL_TEST(ownership_29_11, case_arm_enumeration_suppresses_must_use) {
    auto r = parseAndCheckOwnership(prog(
        "    var q := xs.Where(lambda(x) => x > 0);\n"
        "    case n of\n"
        "      var y => do\n"
        "        for var x in q do\n"
        "          Console.WriteLine($\"{x}\");\n"
        "        end for;\n"
        "      end\n"
        "    end case;\n"
        "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "q"));
}

// The same `case` with the enumeration removed from the arm: the match alone
// spends nothing, so the lint must still fire.
BEZEL_TEST(ownership_29_11, case_arm_without_enumeration_still_draws_must_use) {
    auto r = parseAndCheckOwnership(prog(
        "    var q := xs.Where(lambda(x) => x > 0);\n"
        "    case n of\n"
        "      var y => do\n"
        "        Console.WriteLine($\"{y}\");\n"
        "      end\n"
        "    end case;\n"
        "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "q"));
}

// The EXPRESSION-position `case`: `q.Count()` in an arm spends the right just
// as it does in the statement form. The reachability witness for the CaseExpr
// arm.
BEZEL_TEST(ownership_29_11, case_expression_terminal_suppresses_must_use) {
    auto r = parseAndCheckOwnership(prog(
        "    var q := xs.Where(lambda(x) => x > 0);\n"
        "    return (case n of var y => q.Count(), _ => 0);\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "q"));
}

// §29.11 item 1 again, in the NULL-CONDITIONAL spelling. `q?.Count()` is the
// same consumption point as `q.Count()`; the parser builds a different node
// for it (a CallExpr over a NullCondMemberExpr rather than over a
// MemberAccessExpr), so this is the reachability witness for the second arm of
// `scanLazyConsumptionCallee`. Without that arm this case false-positives
// while the plain spelling above does not.
BEZEL_TEST(ownership_29_11, null_conditional_terminal_suppresses_must_use) {
    auto r = parseAndCheckOwnership(prog(
        "    var q := xs.Where(lambda(x) => x > 0);\n"
        "    return q?.Count() ?? 0;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "q"));
}

// §29.11's suppression set is the CLOSED set of §28.3's, which says nothing
// about body kinds — so the `async` twin behaves identically. This is the
// async-suspend-local report site, a different one from the sync tests above;
// both carry the same warning text, so both are covered.
BEZEL_TEST(ownership_29_11, async_for_in_enumeration_suppresses_must_use) {
    auto r = parseAndCheckOwnership(
        "module M; public class Scanner\n"
        "  public async method Scan(xs: Array<int>): Task<int>\n"
        "    var q := xs.Where(lambda(x) => x > 0);\n"
        "    for var x in q do\n"
        "      Console.WriteLine($\"{x}\");\n"
        "    end for;\n"
        "    return 0;\n"
        "  end Scan;\n"
        "end Scanner;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "q"));
}

// The async control: no consumption point, so the async twin of the lint still
// fires. Without this, the async quiet case above would be satisfied by the
// async site never firing at all — which is exactly the vacuous-green shape.
BEZEL_TEST(ownership_29_11, async_no_consumption_point_still_draws_must_use) {
    auto r = parseAndCheckOwnership(
        "module M; public class Scanner\n"
        "  public async method Scan(xs: Array<int>): Task<int>\n"
        "    var q := xs.Where(lambda(x) => x > 0);\n"
        "    return 0;\n"
        "  end Scan;\n"
        "end Scanner;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "q"));
}