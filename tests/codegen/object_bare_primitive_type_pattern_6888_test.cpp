// Spec-derived regression test for issue #6888.
//
// Spec §19.3 (Patterns) and §19.5, v0.340: a type pattern in a case arm
// matches a value of that type, the same test as `is` (§12.7); "a bare type
// pattern T matches only non-null values" is a NECESSARY condition, not the
// whole test.
//
// THE DEFECT. Over an `Object` scrutinee, a bare primitive type pattern
// (`string`, `int`) in a `case` selected its arm for EVERY non-null value (a
// class instance, an `Array<int>`, a string for `int`): `patternMatches` fell
// through to an unconditional `true`. `o is string` was right; the binding form
// `var n: string` was right. The bare pattern now runs the same runtime test.
//
// Oracle: `runAotChecked` (sanitizers armed, exit 0), exact stdout.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string checkedOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}

// Plain oracle (no leak check) for the rows that pass a boxed primitive to an
// `Object` parameter: a `case` over such a box leaks it, an independent
// defect filed as #6930. The stdout is still compared exactly.
std::string plainOut(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}

}  // namespace

// §19.3 / §19.5: the issue's repro, `string` and `int` forms, over a class
// instance, an `Array<int>` and a string (leak-checked).
BEZEL_TEST(codegen_object_bare_primitive_type_pattern_6888,
           case_expression_over_object_19_3) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; public class D end D; "
            "class C "
            " public static method Probe(o: Object) "
            "   Console.WriteLine(o is string); "
            "   Console.WriteLine((case o of string => 1, _ => 0)); "
            "   Console.WriteLine((case o of int => 1, _ => 0)); "
            " end Probe; "
            " public static method Main(): int "
            "   var d := new D(); var xs := new Array<int>(); "
            "   Probe(d); Probe(xs); Probe(\"hi\"); "
            "   return 0; "
            " end Main; end C;"),
        std::string("False\n0\n0\nFalse\n0\n0\nTrue\n1\n0\n"));
}

// §19.5: a null `Object?` matches neither bare type pattern; the statement form
// of `case` and the expression form agree.
BEZEL_TEST(codegen_object_bare_primitive_type_pattern_6888,
           null_and_statement_form_19_5) {
    BEZEL_EXPECT_EQ(
        plainOut(
            "module M; class C "
            " public static method P(o: Object?): int "
            "   return (case o of int => 1, string => 2, _ => 0); "
            " end P; "
            " public static method Q(o: Object): int "
            "   var r: int := 0; "
            "   case o of string => r := 2, int => r := 1, _ => r := 0 end case; "
            "   return r; "
            " end Q; "
            " public static method Main(): int "
            "   Console.WriteLine(P(null)); Console.WriteLine(P(7)); "
            "   Console.WriteLine(P(\"x\")); Console.WriteLine(Q(7)); "
            "   Console.WriteLine(Q(\"x\")); Console.WriteLine(Q(2.5)); "
            "   return 0; "
            " end Main; end C;"),
        std::string("0\n1\n2\n1\n2\n0\n"));
}
