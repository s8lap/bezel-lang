// fix #6268 (§27.3 *Which block-bodied lambdas return a value*, §22.6
// *Lambda-literal arguments*, v0.339):
//
//   §27.3: "A target type, when present, is checked against this
//    classification — a procedure-bodied lambda does not convert to a
//    value-returning function type, nor the reverse."
//
// #6161 checks a procedure lambda literal against the formal threaded into it,
// but stays silent when that formal came from one member of a same-name set
// (it need not be the member resolution picks). Pre-fix nothing re-ran the
// check once the call was resolved, so a procedure lambda was ACCEPTED at a
// value-returning slot whenever the callee had several same-name candidates:
// all value-returning, of different arity, an override chain, an extension
// that is the only one for its receiver, or a constructor set. The check now
// runs against the formal of the candidate the call / `new` resolves to.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

#define PROC_LAMBDA_MSG_6268                                                  \
    "a procedure-bodied lambda (no path executes a value-carrying `return`) " \
    "does not convert to a function type whose return type is a value"

namespace {

std::string mainCalling6268(const std::string& stmt) {
    return "public static class Program\n"
           "  public static method Main(args: Array<string>): int\n"
           "    " + stmt + "\n"
           "    return 0;\n"
           "  end;\n"
           "end;\n";
}

const char* const kProcLam6268 = "lambda() => do Console.WriteLine(\"x\"); end";

}  // namespace

// Shape 1: every candidate is value-returning (`() -> int`, `() -> string`),
// unqualified and qualified. Col 28 / 30 is the `lambda` keyword.
BEZEL_TEST(procedure_lambda_multi_candidate_value_slot_6268,
           all_value_returning_candidates_rejected_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Over(f: () -> int): string => \"int\";\n"
        "  public static method Over(f: () -> string): string => \"str\";\n"
        "  public static method Main(args: Array<string>): int\n"
        "    Console.WriteLine(Over(" + std::string(kProcLam6268) + "));\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":6:28: " PROC_LAMBDA_MSG_6268));

    auto q = runAot(
        "module T;\n"
        "public static class G\n"
        "  public static method Over(f: () -> int): string => \"int\";\n"
        "  public static method Over(f: () -> string): string => \"str\";\n"
        "end;\n" +
        mainCalling6268("Console.WriteLine(G.Over(" + std::string(kProcLam6268) + "));"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(q));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(q, ":8:30: " PROC_LAMBDA_MSG_6268));
}

// Shape 2: a same-name candidate of a different arity.
BEZEL_TEST(procedure_lambda_multi_candidate_value_slot_6268,
           different_arity_candidate_rejected_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Over(f: () -> int): string => \"int\";\n"
        "  public static method Over(a: int, f: () -> int): string => \"int2\";\n"
        "  public static method Main(args: Array<string>): int\n"
        "    Console.WriteLine(Over(" + std::string(kProcLam6268) + "));\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":6:28: " PROC_LAMBDA_MSG_6268));
}

// Shape 3: an override chain (`virtual` / `override` of one signature).
BEZEL_TEST(procedure_lambda_multi_candidate_value_slot_6268,
           override_chain_rejected_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public class B\n"
        "  public virtual method Over(f: () -> int): string => \"B\";\n"
        "end;\n"
        "public class D : B\n"
        "  public override method Over(f: () -> int): string => \"D\";\n"
        "end;\n" +
        mainCalling6268("var d := new D();\n    Console.WriteLine(d.Over(" +
                        std::string(kProcLam6268) + "));"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":11:30: " PROC_LAMBDA_MSG_6268));
}

// Shape 4: an extension that is the sole candidate for its receiver, beside a
// same-name extension on another type.
BEZEL_TEST(procedure_lambda_multi_candidate_value_slot_6268,
           extension_sole_by_receiver_rejected_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public class K end;\n"
        "public class L end;\n"
        "extension K\n"
        "  public method Over(f: () -> int): string => \"K\";\n"
        "end;\n"
        "extension L\n"
        "  public method Over(f: () -> ()): string => \"L\";\n"
        "end;\n" +
        mainCalling6268("var k := new K();\n    Console.WriteLine(k.Over(" +
                        std::string(kProcLam6268) + "));"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":13:30: " PROC_LAMBDA_MSG_6268));
}

// Constructor sets: all value-returning, and a different arity.
BEZEL_TEST(procedure_lambda_multi_candidate_value_slot_6268,
           constructor_sets_rejected_spec_27_3) {
    for (int arity = 0; arity < 2; ++arity) {
        std::string second = arity ? "  public constructor(a: int, f: () -> int) end;\n"
                                   : "  public constructor(f: () -> string) end;\n";
        auto r = runAot(
            "module T;\n"
            "public class K\n"
            "  public constructor(f: () -> int) end;\n" + second +
            "end;\n" +
            mainCalling6268("var k := new K(" + std::string(kProcLam6268) + ");"));
        BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
        BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":8:20: " PROC_LAMBDA_MSG_6268));
    }
}

// A generic class: `Over(f: () -> T)` on `Box<int>` is a `() -> int` slot at
// this call, beside a different-arity overload.
BEZEL_TEST(procedure_lambda_multi_candidate_value_slot_6268,
           generic_receiver_substituted_value_slot_rejected_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public class Box<T>\n"
        "  public method Over(f: () -> T): string => \"T\";\n"
        "  public method Over(a: int, f: () -> T): string => \"T2\";\n"
        "end;\n" +
        mainCalling6268("var b := new Box<int>();\n    Console.WriteLine(b.Over(" +
                        std::string(kProcLam6268) + "));"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":9:30: " PROC_LAMBDA_MSG_6268));
}

// Two lambdas, each inapplicable in one of the two candidates: no candidate is
// applicable, so the call is rejected.
BEZEL_TEST(procedure_lambda_multi_candidate_value_slot_6268,
           every_candidate_ruled_out_by_some_lambda_rejected_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public static class G\n"
        "  public static method Over(f: () -> int, g: () -> ()): string => \"A\";\n"
        "  public static method Over(f: () -> (), g: () -> int): string => \"B\";\n"
        "end;\n" +
        mainCalling6268("Console.WriteLine(G.Over(" + std::string(kProcLam6268) + ", " +
                        std::string(kProcLam6268) + "));"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, PROC_LAMBDA_MSG_6268));
}

// ACCEPT guards: value lambdas at the same multi-candidate value slots, and a
// procedure lambda at an override chain's `-> ()` slot, still compile and run.
BEZEL_TEST(procedure_lambda_multi_candidate_value_slot_6268,
           value_lambdas_and_unit_slots_still_accepted_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public class B\n"
        "  public virtual method Run(f: () -> ()): string => \"B\";\n"
        "end;\n"
        "public class D : B\n"
        "  public override method Run(f: () -> ()): string\n"
        "    f();\n"
        "    return \"D\";\n"
        "  end;\n"
        "end;\n"
        "public class K\n"
        "  public constructor(f: () -> int) Console.WriteLine(f()); end;\n"
        "  public constructor(a: int, f: () -> int) Console.WriteLine(a + f()); end;\n"
        "end;\n"
        "public static class G\n"
        "  public static method Over(f: () -> int): int => f();\n"
        "  public static method Over(a: int, f: () -> int): int => a + f();\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    Console.WriteLine(G.Over(lambda() => do return 4; end));\n"
        "    Console.WriteLine(G.Over(1, lambda() => 5));\n"
        "    var k := new K(lambda() => do return 2; end);\n"
        "    var d := new D();\n"
        "    Console.WriteLine(d.Run(lambda() => do Console.WriteLine(\"x\"); end));\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n6\n2\nx\nD\n"));
}
