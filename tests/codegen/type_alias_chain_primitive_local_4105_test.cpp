// Spec-derived tests for fix #4105 — §27.8 (v0.300): a chained
// alias-of-primitive declared in the outer-before-inner order
// (`type A := B; type B := int;`) silently lost its resolved type for a
// plain local-variable type annotation — the binding pattern set
// `loc->declType` to the TypeAliasSymbol (not the underlying int), and
// the stale cache was never refreshed for the Type annotation node.
//
// Root cause: `resolveAliasForwardReferences` (fix #4047, §27.8, v0.294)
// re-resolves each top-level alias's own `aliasedType` AST node, fixing
// the alias declaration's cached TypeRef. But the Type annotation node
// that appears in a local's type annotation (the "A" in "var x: A")
// is a DIFFERENT cache key — its entry was set in the first forward
// pass as Kind::Named / TypeAliasSymbol / declaration=nullptr and was
// never refreshed by the alias-transparency arm in `resolveNamed`.
//
// Two fixes are required:
//   1. In `resolveNamed` (TypeResolution_Names.cpp): after following
//      the alias chain, update the cache for BOTH the aliasedType AST
//      node AND the Type node `t` that was passed to resolveNamed, so
//      that `ctx_.resolvedType(t)` returns the fully-resolved type.
//   2. In `ExprTC::declaredTypeOf` (ExprTypecheck.cpp): when the
//      symbol is a LocalSymbol, follow the alias chain on
//      `loc->declType` to handle any stale entries that escaped
//      fix (1) — covers all callers of declaredTypeOf.
//
// Pre-fix (verified FAIL-WITHOUT on r18-c7 @ 96828993):
//   var x: A := 42;
//   var y := x - 1;    // "operator `-` is not defined for type `?`"
//   Console.WriteLine(y.ToString()); // misresolved on B
//
// Post-fix: both compile and run correctly, printing 41.

#include "support/aot_support.hpp"  // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #4105 primary repro: outer-before-inner declaration order.
// var x: A (where A:=B, B:=int) should resolve to int for arithmetic.
BEZEL_TEST(type_alias_chain_primitive_local_4105,
           outer_before_inner_arithmetic) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type A := B; "
        "type B := int; "
        "class C "
        "  public static method Main(): int "
        "    var x: A := 42; "
        "    var y := x - 1; "
        "    return y; "
        "  end Main; "
        "end C;"), 41);
}

// #4105: ToString on the aliased local must resolve correctly.
BEZEL_TEST(type_alias_chain_primitive_local_4105, outer_before_inner_ToString) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type A := B; "
        "type B := int; "
        "class C "
        "  public static method Main(): int "
        "    var x: A := 42; "
        "    Console.WriteLine(x.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}

// #4105: inner-before-inner order (should always have worked; control).
BEZEL_TEST(type_alias_chain_primitive_local_4105,
           inner_before_inner_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type B := int; "
        "type A := B; "
        "class C "
        "  public static method Main(): int "
        "    var x: A := 42; "
        "    var y := x - 1; "
        "    return y; "
        "  end Main; "
        "end C;"), 41);
}

// #4105: single-level alias (should always have worked; control).
BEZEL_TEST(type_alias_chain_primitive_local_4105, single_level_alias_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type Single := int; "
        "class C "
        "  public static method Main(): int "
        "    var x: Single := 42; "
        "    var y := x - 1; "
        "    return y; "
        "  end Main; "
        "end C;"), 41);
}
