// Codegen — §5.1 / §8.6 (v0.313) / #5221: a FUNCTION-VALUE call applies the
// same per-argument implicit conversions a direct call does. The repro is a
// record argument at an interface-typed parameter of a function value
// (`hd.g(new R(1, 1))` with `g: (IShow) -> string`): §8.6 boxes the value; the
// function-value invoke path built its own argument list and passed the raw
// record where a `ptr` was expected (IR verification failure).
//
// §5.1 (v0.313): a value type converts implicitly to an interface it
// implements / to Object (boxing, §8.6); int widens to long / double; `T`
// converts to `T?`. §8.6: the box is a §28.3 unbound owned temporary released
// at the end of the call expression.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotCheckedLenient;

namespace {

const char* kDecls =
    "module M; "
    "interface IShow method Show(): string; end IShow; "
    "record R(A: int, B: int) : IShow "
    "  public method Show(): string return $\"R{A}-{B}\"; end Show; "
    "end R; "
    "class H "
    "  public static method I(x: IShow): string return x.Show(); end I; "
    "  public static method O(x: Object): string "
    "    var r := x as R; return r!.Show(); "
    "  end O; "
    "  public static method D(x: double): double return x * 2.0; end D; "
    "  public static method N(x: int?): int return x ?? -1; end N; "
    "end H; "
    "class Holder "
    "  public g: (IShow) -> string := H.I; "
    "end Holder; ";

void expectOut(const std::string& mainBody, const std::string& expected) {
    std::string src = std::string(kDecls) +
        "class C public static method Main(): int " + mainBody +
        " return 0; end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// per §5.1 / §8.6 (v0.313), issue #5221 repro verbatim: field-stored function.
BEZEL_TEST(function_value_call_arg_conversion_5221, repro_field_stored_function_value) {
    expectOut("var hd := new Holder(); var s2 := hd.g(new R(1, 1)); "
              "Console.WriteLine(s2);", "R1-1\n");
}

// per §5.1 / §8.6 (v0.313): local function value.
BEZEL_TEST(function_value_call_arg_conversion_5221, local_function_value) {
    expectOut("var g: (IShow) -> string := H.I; "
              "Console.WriteLine(g(new R(2, 3)));", "R2-3\n");
}

// per §5.1 / §8.6 (v0.313): record -> Object parameter.
BEZEL_TEST(function_value_call_arg_conversion_5221, record_to_object_parameter) {
    expectOut("var g: (Object) -> string := H.O; "
              "Console.WriteLine(g(new R(4, 5)));", "R4-5\n");
}

// per §5.1 / §8.6 (v0.313): a record local (not a `new` temp) as the argument.
BEZEL_TEST(function_value_call_arg_conversion_5221, record_local_argument) {
    expectOut("var g: (IShow) -> string := H.I; var r := new R(6, 7); "
              "Console.WriteLine(g(r)); Console.WriteLine(r.Show());",
              "R6-7\nR6-7\n");
}

// per §5.1 (v0.313): int -> double widening (control).
BEZEL_TEST(function_value_call_arg_conversion_5221, control_int_widens_to_double) {
    expectOut("var g: (double) -> double := H.D; "
              "Console.WriteLine(g(3));", "6\n");
}

// per §5.1 (v0.313): int -> int? wrap (control).
BEZEL_TEST(function_value_call_arg_conversion_5221, control_int_wraps_to_nullable) {
    expectOut("var g: (int?) -> int := H.N; "
              "Console.WriteLine(g(9));", "9\n");
}

// per §5.1 / §8.6 (v0.313): the direct call is the control.
BEZEL_TEST(function_value_call_arg_conversion_5221, control_direct_call) {
    expectOut("Console.WriteLine(H.I(new R(1, 1)));", "R1-1\n");
}

// per §5.1 / §8.6 (v0.313): 200 boxed calls through a function value all see
// their own argument. NOT a leak oracle: the per-call box is deliberately not
// released (the callee of a function value is unknown, so the #2952 escape
// proof is unavailable; see the over-release guard below), so this row asserts
// values only, sum of len("R{k}-1") for k in 0..199 = 10*4 + 90*5 + 100*6.
BEZEL_TEST(function_value_call_arg_conversion_5221, boxed_argument_loop_200_values) {
    std::string src = std::string(kDecls) +
        "class C public static method Main(): int "
        "  var hd := new Holder(); var t := 0; var k := 0; "
        "  while k < 200 do "
        "    var s := hd.g(new R(k, 1)); t := t + s.Length; k := k + 1; "
        "  end while; "
        "  Console.WriteLine(t); return 0; "
        "end Main; end C;";
    JitResult plain = runAot(src);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, "1090\n");
}

// per §8.6 / §28.3 (v0.313): OVER-RELEASE guard — a function value whose
// callee STORES the boxed parameter into a longer-lived field. A caller-side
// release after the call (MEASURED) freed the stored box: `saved as R` was
// null. Leak detection is off (runAotCheckedLenient) so this pins the
// no-release decision.
BEZEL_TEST(function_value_call_arg_conversion_5221, callee_storing_box_no_use_after_free) {
    std::string src =
        "module M; "
        "interface IShow method Show(): string; end IShow; "
        "record R(A: int) : IShow "
        "  public method Show(): string return $\"R{A}\"; end Show; "
        "end R; "
        "class Sink "
        "  public saved: Object; "
        "  public constructor() saved := null; end constructor; "
        "  public method Keep(x: IShow): string saved := x; return \"k\"; end Keep; "
        "end Sink; "
        "class C public static method Main(): int "
        "  using var s := new Sink(); "
        "  var g: (IShow) -> string := s.Keep; "
        "  var k := 0; "
        "  while k < 50 do g(new R(k)); k := k + 1; end while; "
        "  Console.WriteLine((s.saved as R)!.Show()); return 0; "
        "end Main; end C;";
    JitResult r = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "R49\n");
}
