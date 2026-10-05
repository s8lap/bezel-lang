// Spec-derived regression test for issue #5367 (spec §22.1, v0.314).
//
// THE DEFECT. §22.1 states "the names in one parameter list are distinct."
// The compiler's `collectParams` (SymbolCollection.cpp:147-183) had no duplicate
// check — `method Foo(x: int, x: string)` compiled silently with the second
// parameter winning in scope.
//
// THE FIX. Add a duplicate-within-list check to `collectParams`: track seen names,
// report an error at the second occurrence, but still register the symbol so
// later passes have a well-formed type to resolve against.
//
// SCOPE of this fix: methods, constructors, operators, extern declarations, and
// indexer explicit parameter lists go through `collectParams` and are covered here.
//
// OUT OF SCOPE for this lane:
//   - Primary constructor params (different code path via td.positionalFields)
//   - Lambda destructuring (different code path via MutablyCapturedScan)
//   - Indexer setter's implicit `value` colliding with explicit indexer params
//     (needs a separate check in ExprTypecheck where the value param is appended)
//
// ONE TEST PER collectParams CALL SITE.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"        // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// -----------------------------------------------------------------------------
// Site 1: method parameter list
// -----------------------------------------------------------------------------
BEZEL_TEST(duplicate_param_name_reject_5367, method_params) {
    auto r = runAot(
        "module M;"
        "class C"
        "  public method Foo(x: int, x: string): int return 0; end Foo;"
        "end C;"
        "class P"
        "  public static method Main(): int return 0; end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "duplicate parameter name `x`"));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "parameter list (§22.1"));
}

// -----------------------------------------------------------------------------
// Site 2: constructor parameter list
// -----------------------------------------------------------------------------
BEZEL_TEST(duplicate_param_name_reject_5367, constructor_params) {
    auto r = runAot(
        "module M;"
        "class C"
        "  public constructor(x: int, x: string) end constructor;"
        "end C;"
        "class P"
        "  public static method Main(): int return 0; end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "duplicate parameter name `x`"));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "parameter list (§22.1"));
}

// -----------------------------------------------------------------------------
// Site 3: operator parameter list
// -----------------------------------------------------------------------------
BEZEL_TEST(duplicate_param_name_reject_5367, operator_params) {
    auto r = runAot(
        "module M;"
        "class C"
        "  public static method operator+(x: int, x: string): int;"
        "end C;"
        "class P"
        "  public static method Main(): int return 0; end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "duplicate parameter name `x`"));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "parameter list (§22.1"));
}

// -----------------------------------------------------------------------------
// Site 4: extern declaration parameter list
// -----------------------------------------------------------------------------
BEZEL_TEST(duplicate_param_name_reject_5367, extern_params) {
    auto r = runAot(
        "module M;"
        "extern \"lib\""
        "  public method Foo(x: int, x: string): int;"
        "end;"
        "class P"
        "  public static method Main(): int return 0; end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "duplicate parameter name `x`"));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "parameter list (§22.1"));
}

// -----------------------------------------------------------------------------
// Site 5: indexer explicit parameter list (setter's implicit value is SEPARATE)
// -----------------------------------------------------------------------------
BEZEL_TEST(duplicate_param_name_reject_5367, indexer_explicit_params) {
    auto r = runAot(
        "module M;"
        "class C"
        "  private _v: int := 0;"
        "  public property [x: int, x: string]: int"
        "    get => _v;"
        "    set => do _v := value; end;"
        "  end;"
        "end C;"
        "class P"
        "  public static method Main(): int return 0; end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "duplicate parameter name `x`"));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "parameter list (§22.1"));
}

// -----------------------------------------------------------------------------
// Control: a non-duplicate parameter list must still compile cleanly
// -----------------------------------------------------------------------------
BEZEL_TEST(duplicate_param_name_reject_5367, control_non_duplicate_params) {
    auto r = runAot(
        "module M;"
        "class C"
        "  public constructor(x: int, y: string) end constructor;"
        "  public method Foo(a: int, b: string): int return a; end Foo;"
        "end C;"
        "class P"
        "  public static method Main(): int return 0; end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);  // compiles and runs
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// -----------------------------------------------------------------------------
// Control: lambda `_` is exempt — but `_` is NOT valid as a method parameter name
// (the issue notes that `_` in a lambda is exempt; but `_` as a method parameter
// is a reserved discard token that cannot be declared at all per §1.3).
// So we skip the underscore control — there is no valid test for it here.
// The exemption is only relevant for the lambda-destructure path, which is
// out of scope for this lane's collectParams fix.
// -----------------------------------------------------------------------------