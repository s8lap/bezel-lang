// Regression tests for #1095 (§22.12 / §A.8): a `const` initializer that
// references another const via a QUALIFIED name (`K.V`) was wrongly rejected.
// §A.8 admits a `QualifiedName` (enum constant or named const reference) as a
// compile-time constant. `isConstInitExpr` already accepted a MemberAccessExpr
// whose exprSymbol is a ConstMemberSymbol/EnumConstantSymbol, but the const
// initializer was visited without moduleScope_ reachable, so the class/enum
// qualifier resolved to null and the reference was mis-rejected (same root
// cause as #582/#1088). A qualified NON-const reference (a static field) is
// still correctly rejected. Found by adversarial discovery sweep #7.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1095: a qualified const reference in a const initializer compiles and folds.
BEZEL_TEST(const_qualified_ref_1095, qualified_const_ref_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class K public const V: int := 7; end K; "
        "class C "
        "  public const W: int := K.V + 1; "
        "  public static method Main(): int return C.W; end Main; "
        "end C;"), 8);
}

// Two qualified const references in one initializer.
BEZEL_TEST(const_qualified_ref_1095, two_qualified_const_refs) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class K public const V: int := 7; public const W: int := 3; end K; "
        "class C "
        "  public const X: int := K.V + K.W; "
        "  public static method Main(): int return C.X; end Main; "
        "end C;"), 10);
}

// A qualified NON-const reference (a static field) is still rejected (§22.12).
BEZEL_TEST(const_qualified_ref_1095, qualified_non_const_rejected) {
    auto r = runJit(
        "module M; "
        "class K public static V: int := 7; end K; "
        "class C "
        "  public const X: int := K.V + 1; "
        "  public static method Main(): int return C.X; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// An unqualified bare-name const reference (same class) is unchanged.
BEZEL_TEST(const_qualified_ref_1095, bare_name_const_ref_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public const V: int := 7; "
        "  public const W: int := V + 1; "
        "  public static method Main(): int return C.W; end Main; "
        "end C;"), 8);
}
