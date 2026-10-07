// Codegen — #5241 (follow-up to #4698; spec §2.2, v0.313): "The alias is an
// additional *qualifier*, not a rename". An alias-qualified compiler-built-in
// stdlib type (`G.Stack<int>`, `D.Stopwatch`) must not be captured by a
// same-named TYPE PARAMETER, in sema member lookup (R1) or in generic field
// layout (R3). The bare spelling of the parameter keeps meaning the parameter.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
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
    // #5351 fix: Stopwatch.StartNew() returns owned, so field must be owned.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Diagnostics as D; "
        "class Box<Stopwatch> "
        "  public owned sw: D.Stopwatch; "
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
    // #5351 fix: Stopwatch.StartNew() returns owned, so field must be owned.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Diagnostics as D; "
        "class Box<Stopwatch> "
        "  public owned sw: D.Stopwatch; "
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

// The two failing rows above pinned the ASSIGNMENT-TARGET fetch only. The same
// written-declared-type rule reaches a LOCAL declaration and a RETURN type, and
// each is its own name-keyed type-parameter recognizer in `checkAssignable`, so
// each is pinned separately: a single row cannot distinguish "the rule holds
// everywhere" from "the rule holds at the one position the row exercises".
//
// #5469's own arm is the `#5469` message these rows previously tripped, so
// together the three rows cover #4768 (type-parameter SOURCE), #4810 (primitive
// SOURCE) and #5469 (class/record SOURCE) reading the target side.
BEZEL_TEST(alias_builtin_type_param_capture_5241, spec_2_2_local_declared_type_alias_qualified_stopwatch_not_type_param) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Diagnostics as D; "
        "class Box<Stopwatch> "
        "  public constructor(x: Stopwatch) "
        "    var w: D.Stopwatch := D.Stopwatch.StartNew(); "
        "    Console.WriteLine(w.IsRunning); "
        "  end constructor; "
        "end Box; "
        "class C public static method Main() "
        "  var b := new Box<int>(5); "
        "end Main; end C;"), "True");
}

BEZEL_TEST(alias_builtin_type_param_capture_5241, spec_2_2_return_type_alias_qualified_stopwatch_not_type_param) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Diagnostics as D; "
        "class Box<Stopwatch> "
        "  public method Get(): D.Stopwatch return D.Stopwatch.StartNew(); end Get; "
        "end Box; "
        "class C public static method Main() "
        "  var b := new Box<int>(); "
        "  Console.WriteLine(b.Get().IsRunning); "
        "end Main; end C;"), "True");
}

// #4760 scope, the mirror of the rows above: recognizing `D.Stopwatch` as a
// BUILT-IN must not make it assignable to a primitive. `var n: int := sw;` is
// a heap-reference→value-type store (§5.1 / §4.6, v0.307) and stays rejected.
// This row is the reason the fix exempts the built-in in the SHARED leaf test
// (`refOutParamTypeMentionsTypeParam`) rather than only in the three
// `checkAssignable` shape tests: with the shape tests alone, `sw` stops being
// a "type variable" for the #4760 gate too, and that gate — the only thing left
// rejecting this program — would silently admit it.
//
// The row pins the RULE, not just a non-zero exit: before the fix this program
// was rejected by #4768 reasoning about a TYPE PARAMETER that is not there
// ("`Stopwatch` is a type parameter, and a generic body converts…"). Asserting
// the #4760 wording therefore also pins that the built-in is no longer
// mistaken for the parameter.
BEZEL_TEST(alias_builtin_type_param_capture_5241, spec_5_1_alias_qualified_stopwatch_rejected_at_primitive_target_by_builtin_rule) {
    auto r = runAot(
        "module M; import System.Diagnostics as D; "
        "class Box<Stopwatch> "
        "  public owned sw: D.Stopwatch := D.Stopwatch.StartNew(); "
        "  public method Bad(): int "
        "    var n: int := sw; "
        "    return n; "
        "  end Bad; "
        "end Box; "
        "class C public static method Main() "
        "  var b := new Box<int>(); "
        "  Console.WriteLine(b.Bad()); "
        "end Main; end C;");
    // `!r.ran` alone is ambiguous across four outcomes (see the header of
    // aot_result_asserts.hpp); pair it with the real-location discriminator so
    // this row cannot pass on a crash or a timeout.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "is a heap reference and the target is a value type"));
}
