// Codegen — #5241 (follow-up to #4698; spec §2.2, v0.313): "The alias is an
// additional *qualifier*, not a rename". An alias-qualified compiler-built-in
// stdlib type (`G.Stack<int>`, `D.Stopwatch`) must not be captured by a
// same-named TYPE PARAMETER, in sema member lookup (R1) or in generic field
// layout (R3). The bare spelling of the parameter keeps meaning the parameter.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

// R1: `class Holder<Stack>` + `G.Stack<int>` in a method body.
BEZEL_TEST(alias_builtin_type_param_capture_5241, spec_2_2_member_call_on_alias_qualified_stack_not_type_param) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Collections.Generic as G; "
        "class Holder<Stack> "
        "  public method Make(): int "
        "    var s := new G.Stack<int>(); "
        "    s.Push(3); "
        "    return s.Count; "
        "  end Make; "
        "end Holder; "
        "class C public static method Main() "
        "  var h := new Holder<string>(); "
        "  Console.WriteLine(h.Make()); "
        "end Main; end C;"), "1");
}

// Control: the bare parameter name still means the parameter.
BEZEL_TEST(alias_builtin_type_param_capture_5241, spec_2_2_bare_type_param_named_stack_still_a_parameter) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Collections.Generic as G; "
        "class Holder<Stack> "
        "  public method Id(x: Stack): Stack return x; end Id; "
        "  public method Make(): int "
        "    var s := new G.Stack<int>(); "
        "    s.Push(3); s.Push(4); "
        "    return s.Count; "
        "  end Make; "
        "end Holder; "
        "class C public static method Main() "
        "  var h := new Holder<string>(); "
        "  Console.WriteLine(h.Id(\"z\")); "
        "  Console.WriteLine(h.Make()); "
        "end Main; end C;"), "z\n2");
}

// R3: `class Box<Stopwatch>` with a field typed `D.Stopwatch`.
BEZEL_TEST(alias_builtin_type_param_capture_5241, spec_2_2_generic_field_layout_alias_qualified_stopwatch_not_type_param) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Diagnostics as D; "
        "class Box<Stopwatch> "
        "  public sw: D.Stopwatch; "
        "  public constructor(x: Stopwatch) "
        "    sw := D.Stopwatch.StartNew(); "
        "  end constructor; "
        "end Box; "
        "class C public static method Main() "
        "  var b := new Box<int>(5); "
        "  Console.WriteLine(b.sw.IsRunning); "
        "end Main; end C;"), "True");
}

// Control: the bare parameter name still means the parameter (a method
// signature typed by it takes the substitution).
BEZEL_TEST(alias_builtin_type_param_capture_5241, spec_2_2_generic_field_bare_type_param_named_stopwatch_still_a_parameter) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Diagnostics as D; "
        "class Box<Stopwatch> "
        "  public sw: D.Stopwatch; "
        "  public constructor(x: Stopwatch) "
        "    sw := D.Stopwatch.StartNew(); "
        "  end constructor; "
        "  public method Id(x: Stopwatch): Stopwatch return x; end Id; "
        "end Box; "
        "class C public static method Main() "
        "  var b := new Box<int>(5); "
        "  Console.WriteLine(b.sw.IsRunning); "
        "  Console.WriteLine(b.Id(7)); "
        "end Main; end C;"), "True\n7");
}
