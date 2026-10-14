// fix #6161 (§27.3 "Which block-bodied lambdas return a value", v0.338):
//
//   "A target type, when present, is checked against this classification — a
//    procedure-bodied lambda does not convert to a value-returning function
//    type, nor the reverse."
//
// Pre-fix a procedure-bodied block lambda LITERAL at a `() -> int` target (typed
// local, call argument, explicit generic type argument) was accepted.
// (Not pinned here: `var n: int := w();`, the ()-valued call at an int slot,
// belongs to checkAssignable; see the handback.)

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

#define PROC_LAMBDA_MSG                                                       \
    "a procedure-bodied lambda (no path executes a value-carrying `return`) " \
    "does not convert to a function type whose return type is a value"

// REJECT: typed local. Line 4, col 25 is the `lambda` keyword.
BEZEL_TEST(procedure_lambda_function_typed_target_6161,
           procedure_lambda_literal_at_value_returning_local_rejected_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var f: () -> int := lambda() => do end;\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":4:25: " PROC_LAMBDA_MSG));
}

// REJECT: argument at a `() -> int` parameter (zero-parameter function type).
BEZEL_TEST(procedure_lambda_function_typed_target_6161,
           procedure_lambda_literal_at_value_returning_param_rejected_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Take(f: () -> int)\n"
        "  end;\n"
        "  public static method Main(args: Array<string>): int\n"
        "    Take(lambda() => do end);\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":6:10: " PROC_LAMBDA_MSG));
}

// REJECT: an explicit generic type argument `() -> int` (the qualified-call
// form #6107 forms the lambda at).
BEZEL_TEST(procedure_lambda_function_typed_target_6161,
           procedure_lambda_literal_at_explicit_function_type_argument_rejected_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public static class Util\n"
        "  public static method Id<T>(x: T): T => x;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var g := Util.Id<() -> int>(lambda() => do end);\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":7:33: " PROC_LAMBDA_MSG));
}

// ACCEPT (guards): procedure lambdas at unit targets, value lambdas at value
// targets, zero-parameter, all through the same threaded paths.
BEZEL_TEST(procedure_lambda_function_typed_target_6161,
           matching_procedure_and_value_targets_still_accepted_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Take(f: () -> int): int => f();\n"
        "  public static method TakeU(f: () -> ())\n"
        "    f();\n"
        "  end;\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var a: () -> () := lambda() => do end;\n"
        "    var b: () -> int := lambda() => do return 4; end;\n"
        "    TakeU(lambda() => do return; end);\n"
        "    TakeU(lambda() => do end);\n"
        "    a();\n"
        "    Console.WriteLine(Take(lambda() => do return 5; end) + b());\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("9") != std::string::npos);
}

// ACCEPT (reviewer finding): §22.6 *Lambda-literal arguments* (v0.338): "M(f: ()
// -> ()) beside M(f: () -> int)" is a valid overload set and a procedure-bodied
// lambda is exact at the `-> ()` slot. The target slot threaded from ONE member
// of an overload set must not classify the lambda, in either declaration order.
BEZEL_TEST(procedure_lambda_function_typed_target_6161,
           overloaded_unit_and_value_slots_pick_unit_for_procedure_lambda_spec_22_6) {
    for (int unitFirst = 0; unitFirst < 2; ++unitFirst) {
        std::string overs = unitFirst
            ? "  public static method Over(f: () -> ()): string => \"unit\";\n"
              "  public static method Over(f: () -> int): string => \"int\";\n"
            : "  public static method Over(f: () -> int): string => \"int\";\n"
              "  public static method Over(f: () -> ()): string => \"unit\";\n";
        auto r = runAot(
            "module T;\n"
            "public static class Program\n" + overs +
            "  public static method Main(args: Array<string>): int\n"
            "    Console.WriteLine(Over(lambda() => do Console.WriteLine(\"x\"); end));\n"
            "    return 0;\n"
            "  end;\n"
            "end;\n");
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_TRUE(r.stdoutText.find("unit") != std::string::npos);
    }
}

// ACCEPT, one-parameter pair `(int) -> int` / `(int) -> ()`, both orders.
BEZEL_TEST(procedure_lambda_function_typed_target_6161,
           overloaded_one_param_unit_and_value_slots_spec_22_6) {
    for (int unitFirst = 0; unitFirst < 2; ++unitFirst) {
        std::string overs = unitFirst
            ? "  public static method Over(f: (int) -> ()): string => \"unit\";\n"
              "  public static method Over(f: (int) -> int): string => \"int\";\n"
            : "  public static method Over(f: (int) -> int): string => \"int\";\n"
              "  public static method Over(f: (int) -> ()): string => \"unit\";\n";
        auto r = runAot(
            "module T;\n"
            "public static class Program\n" + overs +
            "  public static method Main(args: Array<string>): int\n"
            "    Console.WriteLine(Over(lambda(x: int) => do Console.WriteLine(\"x\"); end));\n"
            "    return 0;\n"
            "  end;\n"
            "end;\n");
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_TRUE(r.stdoutText.find("unit") != std::string::npos);
    }
}

// ACCEPT, constructor overloads `() -> int` / `() -> ()`: the program is valid
// and must compile and run. Which constructor is picked is NOT pinned here (the
// base picks the `() -> int` one, contrary to §22.6; a separate resolution
// defect, reported).
BEZEL_TEST(procedure_lambda_function_typed_target_6161,
           overloaded_constructors_unit_and_value_slots_compile_spec_22_6) {
    auto r = runAot(
        "module T;\n"
        "public class K\n"
        "  public constructor(f: () -> int) end;\n"
        "  public constructor(f: () -> ()) end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var k := new K(lambda() => do Console.WriteLine(\"x\"); end);\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
}

// REJECT, intentional behaviour flip of lifting the zero-parameter threading
// gate: the existing #1820 / §28.3 producer-row rule (a lambda body's return
// path minting a fresh owned `Dog` at a concrete `Animal` slot, which the
// producer row classifies as a borrow, so nothing would dispose it) now applies
// to a ZERO-parameter target too. The one-parameter form was already rejected.
BEZEL_TEST(procedure_lambda_function_typed_target_6161,
           zero_param_function_target_gets_producer_row_check_spec_28_3) {
    auto r = runAot(
        "module T;\n"
        "public class Animal\n"
        "end;\n"
        "public class Dog: Animal\n"
        "end;\n"
        "public static class Program\n"
        "  public static method B(f: () -> Animal)\n"
        "  end;\n"
        "  public static method Main(args: Array<string>): int\n"
        "    B(lambda() => new Dog());\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":10:19: a lambda body's return path produces a fresh owned value "
           "(`Dog`) at a target slot whose declared return type is `Animal`"));
}

// ACCEPT (reviewer finding): the same §22.6 overload set declared as EXTENSION
// methods, on `string` and on a generic `Array<T>` receiver, both declaration
// orders. The extension call path threads one block member's formal; the set is
// not a sole candidate, so the §27.3 classification must stay silent there.
BEZEL_TEST(procedure_lambda_function_typed_target_6161,
           overloaded_extension_methods_unit_and_value_slots_compile_spec_22_6) {
    for (int unitFirst = 0; unitFirst < 2; ++unitFirst) {
        for (int generic = 0; generic < 2; ++generic) {
            std::string overs = unitFirst
                ? "  public method Over(f: () -> ()): string => \"unit\";\n"
                  "  public method Over(f: () -> int): string => \"int\";\n"
                : "  public method Over(f: () -> int): string => \"int\";\n"
                  "  public method Over(f: () -> ()): string => \"unit\";\n";
            std::string head = generic ? "extension Array<T>\n" : "extension string\n";
            std::string recv = generic ? "    var a: Array<int> := [1];\n"
                                         "    Console.WriteLine(a.Over(lambda() => do Console.WriteLine(\"x\"); end));\n"
                                       : "    Console.WriteLine(\"a\".Over(lambda() => do Console.WriteLine(\"x\"); end));\n";
            auto r = runAot(
                "module T;\n" + head + overs + "end;\n"
                "public static class Program\n"
                "  public static method Main(args: Array<string>): int\n" + recv +
                "    return 0;\n"
                "  end;\n"
                "end;\n");
            BEZEL_EXPECT_TRUE(r.ran);
        }
    }
}

// REJECT: a SOLE extension candidate still gets the §27.3 classification.
BEZEL_TEST(procedure_lambda_function_typed_target_6161,
           sole_extension_candidate_procedure_lambda_at_value_slot_rejected_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "extension string\n"
        "  public method Over(f: () -> int): string => \"int\";\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    Console.WriteLine(\"a\".Over(lambda() => do Console.WriteLine(\"x\"); end));\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, PROC_LAMBDA_MSG));
}

// ACCEPT, base behaviour only: an INTERFACE-declared overload pair (unit and
// value slots), both orders, called through the interface. Which overload is
// picked is deliberately not pinned (§22.6 pick for interface calls is a
// separate resolution defect); only that the program is not newly rejected.
BEZEL_TEST(procedure_lambda_function_typed_target_6161,
           overloaded_interface_methods_unit_and_value_slots_compile_spec_22_6) {
    for (int unitFirst = 0; unitFirst < 2; ++unitFirst) {
        std::string a = "  method Over(f: () -> int): string;\n";
        std::string b = "  method Over(f: () -> ()): string;\n";
        auto r = runAot(
            "module T;\npublic interface I\n" + (unitFirst ? b + a : a + b) + "end;\n"
            "public class C : I\n"
            "  public method Over(f: () -> int): string => \"int\";\n"
            "  public method Over(f: () -> ()): string => \"unit\";\n"
            "end;\n"
            "public static class Program\n"
            "  public static method Main(args: Array<string>): int\n"
            "    var i: I := new C();\n"
            "    Console.WriteLine(i.Over(lambda() => do Console.WriteLine(\"x\"); end));\n"
            "    return 0;\n"
            "  end;\n"
            "end;\n");
        BEZEL_EXPECT_TRUE(r.ran);
    }
}
