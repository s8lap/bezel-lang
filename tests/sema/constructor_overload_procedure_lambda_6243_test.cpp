// fix #6243 (§22.6 *Lambda-literal arguments*, v0.339):
//
//   "the candidate is applicable at the position when the lambda's inferred
//    return type equals the candidate's return slot, implicitly converts to
//    it ([§5.1]), or binds it ...; for a procedure slot (`-> ()`), [§27.3]'s
//    value/procedure classification decides instead — ... a unit-typed body
//    or a `: ()` ascription is exact"
//
// and §27.3 *Which block-bodied lambdas return a value* (v0.339): "a
// procedure-bodied lambda does not convert to a value-returning function type".
//
// So `new K(lambda() => do ... end)` against `constructor(f: () -> int)` beside
// `constructor(f: () -> ())` has exactly one applicable candidate, the `-> ()`
// one, in either declaration order. Pre-fix the function-typed column left
// §22.6 ranking undecided, nothing was published for codegen, and the FIRST
// declared constructor ran — the `() -> int` one, which then called a
// procedure lambda as if it returned an `int` (printing a garbage value).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string ctorPair6243(bool unitFirst, const std::string& intSlot,
                         const std::string& unitSlot) {
    const std::string i =
        "  public constructor(f: " + intSlot + ") Console.WriteLine(\"int\"); end;\n";
    const std::string u =
        "  public constructor(f: " + unitSlot + ") Console.WriteLine(\"unit\"); end;\n";
    return unitFirst ? u + i : i + u;
}

}  // namespace

// The issue's repro (`() -> int` declared first), and the reversed order.
BEZEL_TEST(constructor_overload_procedure_lambda_6243,
           procedure_lambda_selects_unit_constructor_both_orders_spec_22_6) {
    for (int unitFirst = 0; unitFirst < 2; ++unitFirst) {
        auto r = runAot(
            "module T;\n"
            "public class K\n" +
            ctorPair6243(unitFirst != 0, "() -> int", "() -> ()") +
            "end;\n"
            "public static class Program\n"
            "  public static method Main(args: Array<string>): int\n"
            "    var k := new K(lambda() => do Console.WriteLine(\"x\"); end);\n"
            "    return 0;\n"
            "  end;\n"
            "end;\n");
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.stdoutText, std::string("unit\n"));
    }
}

// A `: ()` ascription is exact at the procedure slot too; a named argument
// binds the same way.
BEZEL_TEST(constructor_overload_procedure_lambda_6243,
           ascribed_and_named_procedure_lambda_select_unit_constructor_spec_22_6) {
    auto r = runAot(
        "module T;\n"
        "public class K\n" +
        ctorPair6243(false, "() -> int", "() -> ()") +
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var a := new K(lambda(): () => do Console.WriteLine(\"x\"); end);\n"
        "    var b := new K(f := lambda() => do Console.WriteLine(\"y\"); end);\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("unit\nunit\n"));
}

// One-parameter pair, `(int) -> int` first: the procedure lambda takes the
// `(int) -> ()` constructor, and a value lambda still takes `(int) -> int`.
BEZEL_TEST(constructor_overload_procedure_lambda_6243,
           one_param_procedure_lambda_selects_unit_constructor_spec_22_6) {
    auto r = runAot(
        "module T;\n"
        "public class K\n" +
        ctorPair6243(false, "(int) -> int", "(int) -> ()") +
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var a := new K(lambda(x) => do Console.WriteLine(x); end);\n"
        "    var b := new K(lambda(x) => x + 1);\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("unit\nint\n"));
}

// A generic class: `constructor(f: () -> T)` on `Box<int>` is a `() -> int`
// slot at this `new`, so the procedure lambda is inapplicable there.
BEZEL_TEST(constructor_overload_procedure_lambda_6243,
           generic_class_substituted_value_slot_is_inapplicable_spec_22_6) {
    auto r = runAot(
        "module T;\n"
        "public class Box<T>\n"
        "  public constructor(f: () -> T) Console.WriteLine(\"T\"); end;\n"
        "  public constructor(f: () -> ()) Console.WriteLine(\"unit\"); end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var b := new Box<int>(lambda() => do Console.WriteLine(\"x\"); end);\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("unit\n"));
}

// The selected constructor CALLS the lambda. Pre-fix the `() -> int` body ran
// and printed whatever the procedure lambda left in the return register; now
// the `() -> ()` body runs and only the lambda's own output appears. Looped,
// under the checked (ASan/LSan) oracle. (The lambda captures nothing: a
// capturing ctor-argument lambda followed by a later local leaks 8 B on base
// as well, independent of overloading: #6628.)
BEZEL_TEST(constructor_overload_procedure_lambda_6243,
           selected_unit_constructor_invokes_lambda_checked_spec_22_6) {
    auto r = runAotChecked(
        "module T;\n"
        "public class K\n"
        "  public constructor(f: () -> int) Console.WriteLine(f()); end;\n"
        "  public constructor(f: () -> ()) f(); end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var n := 0;\n"
        "    for var i in 0..<50 do\n"
        "      var k := new K(lambda() => do Console.Write(\"hi\"); end);\n"
        "      n += 1;\n"
        "    end;\n"
        "    Console.WriteLine(\"\");\n"
        "    Console.WriteLine(n);\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    std::string want;
    for (int i = 0; i < 50; ++i) want += "hi";
    want += "\n50\n";
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}
