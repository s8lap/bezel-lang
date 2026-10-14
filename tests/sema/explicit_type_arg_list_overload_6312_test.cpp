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

// The sole candidate an explicit list leaves gets the call check a
// non-overloaded generic method gets (§22.6 stage 3, §5.1), so its
// rejection is that check's diagnostic.
const char* const kSurvivorNeedle =
    "has a type that is not implicitly convertible to the target type (§5.1)";

std::string reject(const std::string& classes, const std::string& body) {
    auto r = runAot("module M;\n" + classes +
                    "public class Program\n"
                    "  public static method Main(): int\n" + body +
                    "    return 0;\n"
                    "  end Main;\n"
                    "end Program;\n");
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kSurvivorNeedle));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    std::string all;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) all += d.message + "\n";
    return all;
}

const char* const kAnimals =
    "public class Animal end Animal;\n"
    "public class Dog: Animal\n"
    "  public method Bark(): int => 7;\n"
    "end Dog;\n";

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
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kSurvivorNeedle));
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
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kSurvivorNeedle));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Review round 2 (#6312), u1: the sole survivor `S<V>(x: Array<V>)` at
// `S<IMark>` does not take a `Marked`; it printed 0 (the generic body run on
// a class instance as if it were an Array). §22.6 stage 3 / §5.1.
BEZEL_TEST(explicit_type_arg_list_overload_6312, sole_survivor_class_argument_to_array_param_rejected_spec_22_6) {
    reject(
        "public interface IMark end IMark;\n"
        "public class Marked: IMark end Marked;\n"
        "public class C\n"
        "  public method S(x: string): int => 1;\n"
        "  public method S<V>(x: Array<V>): int => x.Count;\n"
        "end C;\n",
        "    using var c := new C();\n"
        "    using var m := new Marked();\n"
        "    Console.WriteLine(c.S<IMark>(m));\n");
}

// Review round 2 (#6312), u2: the static sole survivor `F<V>(x: V) where V:
// Dog` at `F<Dog>` given an Animal ran `x.Bark()` on an Animal and printed 7.
// A base-to-derived conversion is never implicit (§5.1).
BEZEL_TEST(explicit_type_arg_list_overload_6312, sole_survivor_base_to_derived_argument_rejected_spec_5_1) {
    const std::string errs = reject(
        std::string(kAnimals) +
        "public class D2\n"
        "  public static method F(x: string): int => 1;\n"
        "  public static method F<V>(x: V): int where V: Dog => x.Bark();\n"
        "end D2;\n",
        "    using var a := new Animal();\n"
        "    Console.WriteLine(D2.F<Dog>(a));\n");
    BEZEL_EXPECT_TRUE(errs.find("downcast") != std::string::npos);
}

// Review round 2 (#6312): a negative constant at the survivor's `ulong`
// parameter does not fit its range (§5.1); it compiled.
BEZEL_TEST(explicit_type_arg_list_overload_6312, sole_survivor_negative_constant_to_ulong_rejected_spec_5_1) {
    const std::string errs = reject(
        "public class C\n"
        "  public method S(x: string): int => 1;\n"
        "  public method S<V>(x: V): int => 10;\n"
        "end C;\n",
        "    using var c := new C();\n"
        "    Console.WriteLine(c.S<ulong>(-1));\n");
    BEZEL_EXPECT_TRUE(errs.find("does not fit the target's range") !=
                      std::string::npos);
}

// Review round 2 (#6312): an int at the survivor's enum parameter; between an
// enum and its underlying integer no conversion is implicit (§5.1).
BEZEL_TEST(explicit_type_arg_list_overload_6312, sole_survivor_int_to_enum_rejected_spec_5_1) {
    const std::string errs = reject(
        "public enum Color\n"
        "  Red, Green;\n"
        "end Color;\n"
        "public class C\n"
        "  public method S(x: string): int => 1;\n"
        "  public method S<V>(x: V): int => 10;\n"
        "end C;\n",
        "    using var c := new C();\n"
        "    Console.WriteLine(c.S<Color>(1));\n");
    BEZEL_EXPECT_TRUE(errs.find("enum and its underlying integer") !=
                      std::string::npos);
}

// Over-rejection guard: the same survivor given a Dog satisfies both the
// argument and the `where V: Dog` constraint and runs.
BEZEL_TEST(explicit_type_arg_list_overload_6312, sole_survivor_applicable_argument_runs_spec_22_6) {
    BEZEL_EXPECT_EQ(run(
        std::string(kAnimals) +
        "public class D2\n"
        "  public static method F(x: string): int => 1;\n"
        "  public static method F<V>(x: V): int where V: Dog => x.Bark();\n"
        "end D2;\n",
        "    using var d := new Dog();\n"
        "    Console.WriteLine(D2.F<Dog>(d));\n"),
        "7\n");
}

// Over-rejection guard (§12.9): an `out` interface parameter is covariant, so
// `IBox<Dog>` reaches `IBox<Animal>` at the survivor (reviewer cov3, with the
// getter returning `owned` so the program is otherwise well-formed).
BEZEL_TEST(explicit_type_arg_list_overload_6312, sole_survivor_covariant_user_interface_runs_spec_12_9) {
    BEZEL_EXPECT_EQ(run(
        std::string(kAnimals) +
        "public interface IBox<out T>\n"
        "  method Get(): owned T;\n"
        "end IBox;\n"
        "public class DBox: IBox<Dog>\n"
        "  public method Get(): owned Dog => new Dog();\n"
        "end DBox;\n"
        "public class C\n"
        "  public method S(x: string): int => 1;\n"
        "  public method S<V>(x: IBox<V>): int => 10;\n"
        "end C;\n",
        "    using var c := new C();\n"
        "    using var d := new DBox();\n"
        "    var b: IBox<Dog> := d;\n"
        "    Console.WriteLine(c.S<Animal>(b));\n"),
        "10\n");
}

// Over-rejection guard, reviewer cov3 as written for two generic candidates
// (getter `owned`, as above): the covariant argument keeps the first.
BEZEL_TEST(explicit_type_arg_list_overload_6312, covariant_user_interface_two_candidates_runs_spec_12_9) {
    BEZEL_EXPECT_EQ(run(
        std::string(kAnimals) +
        "public interface IBox<out T>\n"
        "  method Get(): owned T;\n"
        "end IBox;\n"
        "public class DBox: IBox<Dog>\n"
        "  public method Get(): owned Dog => new Dog();\n"
        "end DBox;\n"
        "public class C\n"
        "  public method S<V>(x: IBox<V>): int => 10;\n"
        "  public method S<V>(x: IBox<IBox<V>>): int => 20;\n"
        "end C;\n",
        "    using var c := new C();\n"
        "    using var d := new DBox();\n"
        "    var b: IBox<Dog> := d;\n"
        "    Console.WriteLine(c.S<Animal>(b));\n"),
        "10\n");
}

// Over-rejection guard: an `Array<Dog>` reaches the survivor's
// `IEnumerable<Animal>` (Array implements `IEnumerable<out T>`, §12.9), and a
// widening or fitting constant reaches its primitive parameter (§5.1). The
// base rejected both, ranking the non-generic `S(x: string)` too.
BEZEL_TEST(explicit_type_arg_list_overload_6312, sole_survivor_implicit_conversions_run_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kAnimals) +
        "public class C\n"
        "  public method S(x: string): int => 1;\n"
        "  public method S<V>(x: V): int => 10;\n"
        "  public method T(x: string): int => 1;\n"
        "  public method T<V>(x: IEnumerable<V>): int => 20;\n"
        "end C;\n",
        "    using var c := new C();\n"
        "    using var l := new Array<Dog>();\n"
        "    var i: int := 3;\n"
        "    Console.WriteLine(c.T<Animal>(l));\n"
        "    Console.WriteLine(c.S<long>(i));\n"
        "    Console.WriteLine(c.S<byte>(7));\n"),
        "20\n10\n10\n");
}
