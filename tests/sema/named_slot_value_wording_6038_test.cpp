// Regression test for issue #6038 (sema, spec §19.3 L3951, v0.338). Wording
// only: the diagnostic for a non-const bare name in a NAMED slot.
//
// §19.3 "Named destructuring" (L3951): "a binding in a named slot is spelled
// `var w`". A non-const local there (`Rect(Width = q)`) is not a binding and is
// rejected. The message used to be the top-level text ("cannot be a top-level
// `case` pattern ... use a guard"), which points at the wrong fix in a named
// slot. The named-slot message now names the slot-binding spelling `var q`.
//
// The top-level message is unchanged (pinned by tests/lit/
// case_toplevel_named_pattern_value_reject_1847.bzl).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

BEZEL_TEST(named_slot_value_wording_6038,
           named_slot_local_value_names_var_spelling_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var q := 5; var b := new Rect(5, 4); "
        "    case b of Rect(Width = q) => Console.WriteLine(\"eq\");, "
        "      _ => Console.WriteLine(\"n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`q` is a value, not a type or constant"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a binding there is spelled `var q`"));
}

BEZEL_TEST(named_slot_value_wording_6038,
           nested_named_slot_local_value_names_var_spelling_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Inner(X: int); "
        "public record Outer(I: Inner); "
        "public class Program "
        "  public static method Main() "
        "    var q := 5; var b := new Outer(new Inner(5)); "
        "    case b of Outer(I = Inner(X = q)) => Console.WriteLine(\"eq\");, "
        "      _ => Console.WriteLine(\"n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a binding there is spelled `var q`"));
}
