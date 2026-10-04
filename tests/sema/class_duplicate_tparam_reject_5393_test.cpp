// Spec-derived regression test for issue #5393 (spec §14.1, v0.314).
//
// THE DEFECT. §14.1 states "the names in one type-parameter list are
// distinct." The compiler enforced this for METHOD type-parameter lists
// (resolveTopDecl's method arm, TypeResolution_Names.cpp ~line 1481) but NOT
// for CLASS/INTERFACE/RECORD type-parameter lists (resolveTypeDecl, same file).
// The result: `class Box<T, T>` compiles silently, creating two type
// parameters both named `T` — unsound: instantiations cannot distinguish them,
// and name mangling collapses them.
//
// THE FIX. Add the same duplicate-within-list check to resolveTypeDecl's
// type-parameter registration loop, and to bindExtensionTargetTypeParams for
// extension blocks.
//
// ONE TEST PER SITE (class / interface / nested / extension).

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"        // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// -----------------------------------------------------------------------------
// Site 1: class type-parameter list
// -----------------------------------------------------------------------------
BEZEL_TEST(class_duplicate_tparam_reject_5393, class_tparam_list) {
    auto r = runAot(
        "module M;"
        "class Box<T, T>"
        "  public constructor() end constructor;"
        "end Box;"
        "class P"
        "  public static method Main(): int return 0; end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "duplicate type parameter name `T`"));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "type-parameter list (§14.1"));
}

// -----------------------------------------------------------------------------
// Site 2: interface type-parameter list
// -----------------------------------------------------------------------------
BEZEL_TEST(class_duplicate_tparam_reject_5393, interface_tparam_list) {
    auto r = runAot(
        "module M;"
        "interface I<T, T>"
        "end I;"
        "class P"
        "  public static method Main(): int return 0; end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "duplicate type parameter name `T`"));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "type-parameter list (§14.1"));
}

// -----------------------------------------------------------------------------
// Site 3: nested type inside a generic class
// -----------------------------------------------------------------------------
BEZEL_TEST(class_duplicate_tparam_reject_5393, nested_class_tparam_list) {
    auto r = runAot(
        "module M;"
        "class Outer<T>"
        "  public class Inner<T, T>"
        "  end Inner;"
        "end Outer;"
        "class P"
        "  public static method Main(): int return 0; end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "duplicate type parameter name `T`"));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "type-parameter list (§14.1"));
}

// -----------------------------------------------------------------------------
// Site 4: extension block type-parameter list
// (extension's type params come from the target type's generic args)
// -----------------------------------------------------------------------------
BEZEL_TEST(class_duplicate_tparam_reject_5393, extension_tparam_list) {
    auto r = runAot(
        "module M;"
        "extension Array<T, T>"
        "end extension;"
        "class P"
        "  public static method Main(): int return 0; end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "duplicate type parameter name `T`"));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "type-parameter list (§14.1"));
}

// -----------------------------------------------------------------------------
// Control: a non-duplicate generic class must still compile cleanly
// -----------------------------------------------------------------------------
BEZEL_TEST(class_duplicate_tparam_reject_5393, control_non_duplicate_tparams) {
    auto r = runAot(
        "module M;"
        "class Box<T, U>"
        "  public constructor() end constructor;"
        "end Box;"
        "class P"
        "  public static method Main(): int return 0; end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);  // compiles and runs
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
