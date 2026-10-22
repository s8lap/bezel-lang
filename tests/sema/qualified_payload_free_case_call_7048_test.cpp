// #7048 -- spec §35.3, v0.340: "Construction is qualified, resolving exactly
// as enum constants do (§10.1): `FetchOutcome.NotModified` for a payload-free
// case (a value expression, not a call)". An argument list after a qualified
// payload-free case (`Outcome.Nothing()`) has no meaning and is the same
// arity error §35.3/§35.5 name for the bare spelling (#2354): one error,
// "case `Outcome.Nothing` has no payload, so it takes no argument list", with
// the remedy `Outcome.Nothing`. Owner ruling on #7048 (Reading 1).
//
// MEASURED on the base (a8f89d78): the issue program compiled and printed
// `nothing`. The reject rows fail without the fix; rows marked CONTROL pass on
// both sides.

#include <regex>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageCount;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

const char kNeedle[] = "has no payload, so it takes no argument list";

// Number of `<file>:<line>:<col>: ` diagnostic lines in the captured stderr.
int locatedLines(const bezel::test::JitResult& r) {
    static const std::regex line(":[0-9]+:[0-9]+: ");
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        for (auto it = std::sregex_iterator(d.message.begin(), d.message.end(),
                                            line);
             it != std::sregex_iterator(); ++it)
            ++n;
    return n;
}

// Exactly one error, with the qualified text, remedy and the wanted line:col.
void expectOneArityError(const std::string& src, const char* caseName,
                         int line, int col) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, kNeedle), 1);
    BEZEL_EXPECT_EQ(locatedLines(r), 1);
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, "not callable"), 0);
    const std::string loc = ":" + std::to_string(line) + ":" +
                            std::to_string(col) + ": case `" +
                            caseName + "` " + kNeedle +
                            ": a payload-free case is the value `" + caseName +
                            "`, not a call";
    BEZEL_EXPECT_TRUE(diagnosticMessageCount(r, loc.c_str()) == 1);
}

void expectOut(const std::string& src, const char* want) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

const char kHead[] =
    "module QN;\n"
    "public choice Outcome Got(b: string), Nothing; end Outcome;\n"
    "public class Program\n"
    "  public static method Show(o: Outcome): string\n"
    "    return (case o of Got(var b) => b, Nothing => \"nothing\");\n"
    "  end Show;\n";

}  // namespace

// The issue's program, verbatim (initializer position).
BEZEL_TEST(sema_qualified_payload_free_7048, spec_35_3_issue_program_rejected) {
    expectOneArityError(
        "module QN;\n"
        "public choice Outcome Got(b: string), Nothing; end Outcome;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var o := Outcome.Nothing();\n"
        "    Console.WriteLine((case o of Got(var b) => b, Nothing => "
        "\"nothing\"));\n"
        "  end Main;\n"
        "end Program;\n",
        "Outcome.Nothing", 5, 14);
}

BEZEL_TEST(sema_qualified_payload_free_7048, spec_35_3_return_position_rejected) {
    expectOneArityError(std::string(kHead) +
                            "  public static method P(): Outcome\n"
                            "    return Outcome.Nothing();\n"
                            "  end P;\n"
                            "  public static method Main() end Main;\n"
                            "end Program;\n",
                        "Outcome.Nothing", 8, 12);
}

BEZEL_TEST(sema_qualified_payload_free_7048, spec_35_3_argument_position_rejected) {
    expectOneArityError(std::string(kHead) +
                            "  public static method Main()\n"
                            "    Console.WriteLine(Show(Outcome.Nothing()));\n"
                            "  end Main;\n"
                            "end Program;\n",
                        "Outcome.Nothing", 8, 28);
}

BEZEL_TEST(sema_qualified_payload_free_7048, spec_35_3_collection_element_rejected) {
    expectOneArityError(std::string(kHead) +
                            "  public static method Main()\n"
                            "    var xs: Array<Outcome> := [Outcome.Got(\"x\"), "
                            "Outcome.Nothing()];\n"
                            "  end Main;\n"
                            "end Program;\n",
                        "Outcome.Nothing", 8, 50);
}

BEZEL_TEST(sema_qualified_payload_free_7048, spec_35_3_generic_choice_rejected) {
    expectOneArityError(
        "module QN;\n"
        "public choice Opt<T> Yes(v: T), No; end Opt;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var o := Opt<int>.No();\n"
        "  end Main;\n"
        "end Program;\n",
        "Opt.No", 5, 14);
}

// CONTROL: the value spelling, a payload case, and the bare declared-result
// `return Nothing;` all stay accepted.
BEZEL_TEST(sema_qualified_payload_free_7048, control_value_spelling_accepted) {
    expectOut(std::string(kHead) +
                  "  public static method Main()\n"
                  "    Console.WriteLine(Show(Outcome.Nothing));\n"
                  "  end Main;\n"
                  "end Program;\n",
              "nothing\n");
}

BEZEL_TEST(sema_qualified_payload_free_7048, control_payload_case_accepted) {
    expectOut(std::string(kHead) +
                  "  public static method Main()\n"
                  "    Console.WriteLine(Show(Outcome.Got(\"x\")));\n"
                  "  end Main;\n"
                  "end Program;\n",
              "x\n");
}

BEZEL_TEST(sema_qualified_payload_free_7048, control_bare_return_accepted) {
    expectOut(std::string(kHead) +
                  "  public static method P(): Outcome\n"
                  "    return Nothing;\n"
                  "  end P;\n"
                  "  public static method Main()\n"
                  "    Console.WriteLine(Show(P()));\n"
                  "  end Main;\n"
                  "end Program;\n",
              "nothing\n");
}

// CONTROL: a static method sharing a case's name on a different type is a
// method call, not a case construction.
BEZEL_TEST(sema_qualified_payload_free_7048, control_same_named_static_method) {
    expectOut(std::string(kHead) +
                  "  public static method Main()\n"
                  "    Console.WriteLine(Other.Nothing());\n"
                  "  end Main;\n"
                  "end Program;\n"
                  "public class Other\n"
                  "  public static method Nothing(): string return \"m\"; end "
                  "Nothing;\n"
                  "end Other;\n",
              "m\n");
}

// CONTROL: a case whose payload is function-typed: invoking the extracted
// value is an ordinary call and stays accepted.
BEZEL_TEST(sema_qualified_payload_free_7048, control_function_payload_invoked) {
    expectOut(
        "module QN;\n"
        "public choice Act Run(f: () -> string), Idle; end Act;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var a := Act.Run(lambda() => \"ran\");\n"
        "    Console.WriteLine((case a of Run(var f) => f(), Idle => \"idle\"));\n"
        "  end Main;\n"
        "end Program;\n",
        "ran\n");
}
