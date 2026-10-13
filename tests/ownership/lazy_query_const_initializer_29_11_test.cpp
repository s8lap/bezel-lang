// Spec-derived regression tests for §29.11 (v0.334) *Multiple enumeration*,
// the must-use suppression, over a consumption point spelled inside a `const`
// declaration's initializer.
//
// THE BUG (ratchet #3591 family, "a walker spelled for one binding form
// silently no-ops for the other"). `scanLazyConsumptionStmt` walked a
// statement list with an `else if` chain over statement kinds whose local-
// declaration arm was spelled for `ast::LocalDecl` only:
//
//     } else if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
//       scanLazyConsumptionExpr(ld->initializer.get(), ctx, out);
//
// `ast::LocalConstDecl` is a SIBLING A.6 node, not a derived class (A.6:
// `LocalConstDecl ::= "const" Identifier (":" Type)? ":=" ConstExpr ";"`), so
// that cast MISSES every `const` declaration statement and the walker
// silently no-opped for the whole binding form — the §29.11 suppression set
// was never populated from a `const` initializer.
//
// WHY THAT IS A REAL FALSE POSITIVE, not a theoretical gap:
//
//   * §29.11's normative consumption-point list is about the USE of the query,
//     not the binding form of the declaration the use happens to sit in.
//     Item 1 spends a bound query's enumeration right at "Enumeration or a
//     terminal operator (`for`, `Count`, `First`, `ToArray`, ...)".
//   * §21.1 (Runtime `const` locals) ADMITS a value-typed `const` whose
//     initializer is exactly such an operator: `const n := q.Count();` is a
//     runtime const of type `int`, not one of the rejected owned productions.
//     §21.1 rejects only a `const` initializer that IS itself the query ("a
//     lazy query likewise, its §29.11 single-consumption discipline riding
//     the minted frame") — it does not reject a const that SPENDS one.
//
// So `const n := q.Count();` spends `q`'s right, and §29.11 says the must-use
// warning "is suppressed where the binding's enumeration right is spent at a
// statically visible consumption point". Pre-fix the walker missed the
// statement, the suppression never fired, and the lint drew a warning on a
// query whose right had been spent.
//
// Each test asserts `!hasErrors()` on the quiet rows: a program that failed to
// parse or typecheck would emit no warning for the trivial reason that the
// binding was never analysed, which would make the quiet assertion vacuous
// (the row would be green for the wrong reason). The mirror rows assert the
// warning BY NAME (`q`) and are the controls that prove the fix suppressed
// this lint rather than disabling it.
//
// NOT A DISPOSAL CHANGE. The new arm only ADDS spent rights, and every
// consumer of the set is a `continue` on a diagnostic (the two must-use
// suppressions and the §28.3 hard-reject skip), so the arm can only REMOVE
// diagnostics. `autoDropAtScopeEnd` is still set — §29.11's "the auto-drop
// still running" is untouched.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::parseAndCheckOwnership;

namespace {

// The standard single-class compilation unit; `xs` is a plain `Array<int>`
// parameter, so it carries no §28.3 obligation and cannot stand in for `q`.
std::string prog(const std::string& body) {
    return "module M; public class Scanner "
           "  public method Scan(xs: Array<int>): int\n" +
           body +
           "  end Scan;\n"
           "end Scanner;\n";
}

}  // namespace

// THE REPRO. Byte-identical to `terminal_operator_suppresses_must_use` in
// lazy_query_consumption_point_29_11_test.cpp except that the consuming
// declaration is spelled `const` instead of `var`. §21.1 admits it (an `int`
// initializer is a value, not a rejected owned production), §29.11's item 1
// spends `q`'s right on `q.Count()`, and the warning must therefore be
// suppressed — the same verdict the `var` spelling already got.
//
// FAILS WITHOUT THE FIX: pre-fix this draws the §28.3 must-use warning.
BEZEL_TEST(ownership_29_11, const_initializer_terminal_operator_suppresses_must_use) {
    auto r = parseAndCheckOwnership(prog(
        "    var q := xs.Where(lambda(x) => x > 0);\n"
        "    const n := q.Count();\n"
        "    return n;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "q"));
}

// THE MIRROR, and the control the fix must not weaken: the same `const`
// declaration with a consumption point that does NOT spend `q`'s right (a
// value-typed read of an element), so the lint MUST still fire, naming `q`.
// This is what proves the new arm suppressed the lint by seeing the
// consumption point, not by going silent on `const` declarations wholesale.
BEZEL_TEST(ownership_29_11, const_initializer_without_consumption_still_warns) {
    auto r = parseAndCheckOwnership(prog(
        "    var q := xs.Where(lambda(x) => x > 0);\n"
        "    const first := xs[0];\n"
        "    return first;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "q"));
}

// The `var` control, re-pinned here so this file stands alone: the pre-existing
// behaviour the fix must leave untouched.
BEZEL_TEST(ownership_29_11, var_initializer_terminal_operator_still_suppresses) {
    auto r = parseAndCheckOwnership(prog(
        "    var q := xs.Where(lambda(x) => x > 0);\n"
        "    var n := q.Count();\n"
        "    return n;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "q"));
}

// §21.1 still rejects a `const` BOUND TO the query — the fix is about where a
// consumption point is spelled, not about admitting `const` query bindings.
// Pinning the rejection here keeps the two rules from being confused: the
// §29.11 suppression must not become a back door into `const q := xs.Where()`.
BEZEL_TEST(ownership_29_11, const_bound_to_query_still_rejected_by_21_1) {
    auto r = parseAndCheckOwnership(prog(
        "    var xs2 := new Array<int>(1);\n"
        "    const q := xs2.Where(lambda(x) => x > 0);\n"
        "    return q.Count();\n"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        bezel::test::hasErrorMessage(r, "must not take an owned production"));
}