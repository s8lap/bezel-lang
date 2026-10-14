// #6312 (§14.5, §22.6, v0.338): an explicit type-argument list on a call to an overloaded name. "The list's length selects first: a non-generic candidate takes no list", so `c.S<int>(1)` beside `S(x: int)` and `S<V>(x: V)` reaches only the generic one; and "when a full explicit list is supplied, no inference runs" and "when none of the candidates applies, the call is an error", so a list whose substituted parameter types fit no candidate is rejected (it compiled and ran the first declaration). Conformance rows s14_5_0012 / s14_5_0010.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

std::string run(const std::string& classes, const std::string& body) {
    auto r = runAot("module M;\n" + classes +
                    "public class Program\n"
                    "  public static method Main(): int\n" + body +
                    "    return 0;\n"
                    "  end Main;\n"
                    "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    return r.stdoutText;
}

}  // namespace

// s14_5_0012: the list reaches only the generic candidate; no list prefers the non-generic one (§22.6).
BEZEL_TEST(explicit_type_arg_list_overload_6312, list_skips_non_generic_candidate_spec_14_5) {
    BEZEL_EXPECT_EQ(run(
        "public class C\n"
        "  public method S(x: int): int => 5;\n"
        "  public method S<V>(x: V): int => 10;\n"
        "end C;\n",
        "    using var c := new C();\n"
        "    Console.WriteLine($\"{c.S<int>(1)} {c.S(1)} {c.S<string>(\"a\")}\");\n"),
        "10 5 10\n");
}

// The list length picks between two generic candidates by their type-parameter count.
BEZEL_TEST(explicit_type_arg_list_overload_6312, list_length_selects_among_generic_candidates_spec_14_5) {
    BEZEL_EXPECT_EQ(run(
        "public class C\n"
        "  public method S<A>(x: A, y: string): int => 1;\n"
        "  public method S<A, B>(x: A, y: B): int => 2;\n"
        "  public method S(x: int, y: string): int => 3;\n"
        "end C;\n",
        "    using var c := new C();\n"
        "    Console.WriteLine($\"{c.S<int>(1, \"a\")} {(c.S<int, string>(1, \"a\"))} {c.S(1, \"a\")}\");\n"),
        "1 2 3\n");
}

// A static method: the list reaches the generic overload, the plain call the non-generic one.
BEZEL_TEST(explicit_type_arg_list_overload_6312, static_list_skips_non_generic_candidate_spec_14_5) {
    BEZEL_EXPECT_EQ(run(
        "public class U\n"
        "  public static method F(x: int): int => 5;\n"
        "  public static method F<V>(x: V): int => 10;\n"
        "end U;\n",
        "    Console.WriteLine($\"{U.F<int>(1)} {U.F(1)}\");\n"),
        "10 5\n");
}

// s14_5_0010: no candidate's substituted parameter types apply to the argument.
BEZEL_TEST(explicit_type_arg_list_overload_6312, list_fits_no_candidate_rejected_spec_14_5) {
    auto r = runAot(R"(
module M;
public class C
  public method S<V>(x: Array<V>): int => 10;
  public method S<V>(x: Array<Array<V>>): int => 20;
end C;
public class Program
  public static method Main(): int
    using var c := new C();
    using var b := new Array<Array<int>>();
    Console.WriteLine(c.S<string>(b));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "no overload of `S` is applicable"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The reject reaches a primitive position too: both candidates' second parameter is `int`, the argument a string.
BEZEL_TEST(explicit_type_arg_list_overload_6312, list_primitive_mismatch_every_candidate_rejected_spec_22_6) {
    auto r = runAot(R"(
module M;
public class C
  public method S<V>(x: Array<V>, n: int): int => 10;
  public method S<V>(x: Array<Array<V>>, n: int): int => 20;
end C;
public class Program
  public static method Main(): int
    using var c := new C();
    using var b := new Array<int>();
    Console.WriteLine(c.S<int>(b, "s"));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "no overload of `S` is applicable"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Guard against over-rejection: with one applicable candidate the list call still compiles and picks it.
BEZEL_TEST(explicit_type_arg_list_overload_6312, list_with_one_applicable_candidate_accepted_spec_22_6) {
    BEZEL_EXPECT_EQ(run(
        "public class C\n"
        "  public method S<V>(x: Array<V>): int => 10;\n"
        "  public method S<V>(x: Array<Array<V>>): int => 20;\n"
        "end C;\n",
        "    using var c := new C();\n"
        "    using var a := new Array<string>();\n"
        "    using var b := new Array<Array<string>>();\n"
        "    Console.WriteLine($\"{c.S<string>(a)} {c.S<string>(b)}\");\n"),
        "10 20\n");
}

// Review (#6312): an interface instantiation may be covariant (§11.3: IEnumerable<Dog> is assignable to IEnumerable<Animal>), so the list `<Animal>` does not make `S<V>(IEnumerable<V>)` inapplicable. Must run and print 10.
BEZEL_TEST(explicit_type_arg_list_overload_6312, covariant_interface_argument_still_applies_spec_11_3) {
    BEZEL_EXPECT_EQ(run(
        "public class Animal end Animal;\n"
        "public class Dog: Animal end Dog;\n"
        "public class C\n"
        "  public method S<V>(x: IEnumerable<V>): int => 10;\n"
        "  public method S<V>(x: IEnumerable<IEnumerable<V>>): int => 20;\n"
        "end C;\n",
        "    using var c := new C();\n"
        "    using var l := new Array<Dog>();\n"
        "    var e: IEnumerable<Dog> := l;\n"
        "    Console.WriteLine(c.S<Animal>(e));\n"),
        "10\n");
}

// Review (#6312): an Array is invariant, so Array<Dog> does not reach Array<Animal>: rejected.
BEZEL_TEST(explicit_type_arg_list_overload_6312, invariant_array_argument_rejected_spec_22_6) {
    auto r = runAot(R"(
module M;
public class Animal end Animal;
public class Dog: Animal end Dog;
public class C
  public method S<V>(x: Array<V>): int => 10;
  public method S<V>(x: Array<Array<V>>): int => 20;
end C;
public class Program
  public static method Main(): int
    using var c := new C();
    using var l := new Array<Dog>();
    Console.WriteLine(c.S<Animal>(l));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "no overload of `S` is applicable"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Review (#6312): the list narrows to the sole generic candidate, which does not accept the string argument: rejected, never the generic body run on a string.
BEZEL_TEST(explicit_type_arg_list_overload_6312, sole_survivor_not_applicable_rejected_spec_14_5) {
    auto r = runAot(R"(
module M;
public class C
  public method S(x: string): string => "nongen " + x;
  public method S<V>(x: V): string => "gen";
end C;
public class Program
  public static method Main(): int
    using var c := new C();
    Console.WriteLine(c.S<int>("abc"));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "no overload of `S` is applicable"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Same for a nested own-parameter survivor given a flat Array<int>.
BEZEL_TEST(explicit_type_arg_list_overload_6312, sole_survivor_array_not_applicable_rejected_spec_14_5) {
    auto r = runAot(R"(
module M;
public class C
  public method S(x: Array<int>): int => 5;
  public method S<V>(x: Array<Array<V>>): int => 20;
end C;
public class Program
  public static method Main(): int
    using var c := new C();
    using var a := new Array<int>();
    Console.WriteLine(c.S<int>(a));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "no overload of `S` is applicable"));
    BEZEL_EXPECT_FALSE(r.ran);
}
