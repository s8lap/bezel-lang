// Regression tests for #7703 -- a user extension call on a built-in
// `StringBuilder` receiver evaluated the receiver expression TWICE: the
// StringBuilder arm of tryLowerCollectionInstanceCall lowered the receiver,
// declined the member it does not declare, and the extension lowering lowered
// the receiver again.
//
// Language spec §22.5 (v0.350): an operand is evaluated once, in written
// order. §26.7 (v0.350): "The receiver is always a borrow" -- of the one value
// the receiver expression produced. §26.5 rule 1/2 (v0.350): an instance
// member the receiver's declared type declares commits the call to the
// built-in; any other name selects the extension. stdlib §3.1 lists the
// StringBuilder members (Length, Capacity, Append, AppendLine, AppendFormat,
// Insert, Remove, Replace, Clear, ToString).
//
// The extension stores nothing and the fresh `new StringBuilder` receiver is
// not claimed for disposal when the terminal is a user extension (#7565 gate),
// so every row runs on runAotCheckedLenient: generated-code ASan stays ON,
// LeakSanitizer is OFF (the receiver keeps its pre-fix leak, the sound
// direction).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotCheckedLenient;

namespace {

void expectClean(const JitResult& r, const char* out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// `Tag` prints its argument, so each evaluation of an operand is observable.
const char* kPrelude =
    "module M;\n"
    "extension StringBuilder\n"
    "  public method Twice(): int => this.Length * 2;\n"
    "  public method Join(a: string, b: string): string\n"
    "    return a + b + this.ToString();\n"
    "  end Join;\n"
    "  public method Mark()\n"
    "    this.Append(\"!\");\n"
    "  end Mark;\n"
    "end;\n"
    "class C\n"
    "  public static method Tag(s: string): string\n"
    "    Console.WriteLine(\"eval \" + s);\n"
    "    return s;\n"
    "  end Tag;\n";

std::string program(const std::string& body) {
    return std::string(kPrelude) +
        "  public static method Main(): int\n" + body +
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

}  // namespace

BEZEL_TEST(stringbuilder_extension_receiver_once_7703,
           receiver_chain_operand_evaluated_once_spec_22_5) {
    // The issue's repro shape: a side-effecting operand inside the receiver
    // chain of an extension call. Before the fix "eval b" printed twice.
    expectClean(
        runAotCheckedLenient(program(
            "    var n := new StringBuilder(\"a\").Append(C.Tag(\"b\")).Twice();\n"
            "    Console.WriteLine(n);\n")),
        "eval b\n4\n");
}

BEZEL_TEST(stringbuilder_extension_receiver_once_7703,
           receiver_constructor_argument_evaluated_once_spec_22_5) {
    expectClean(
        runAotCheckedLenient(program(
            "    var n := new StringBuilder(C.Tag(\"r\")).Twice();\n"
            "    Console.WriteLine(n);\n")),
        "eval r\n2\n");
}

BEZEL_TEST(stringbuilder_extension_receiver_once_7703,
           receiver_then_arguments_each_evaluated_once_in_written_order_spec_22_5) {
    expectClean(
        runAotCheckedLenient(program(
            "    var s := new StringBuilder(C.Tag(\"r\")).Join(C.Tag(\"a\"), C.Tag(\"b\"));\n"
            "    Console.WriteLine(s);\n")),
        "eval r\neval a\neval b\nabr\n");
}

BEZEL_TEST(stringbuilder_extension_receiver_once_7703,
           receiver_evaluated_once_in_whole_statement_form_spec_22_5) {
    expectClean(
        runAotCheckedLenient(program(
            "    new StringBuilder(C.Tag(\"r\")).Append(C.Tag(\"s\")).Mark();\n")),
        "eval r\neval s\n");
}

BEZEL_TEST(stringbuilder_extension_receiver_once_7703,
           bound_receiver_extension_still_dispatches_spec_26_5_rule_2) {
    // A bound receiver (no side effect to count): the extension still runs on
    // the builder itself, so its mutation is visible to the caller.
    expectClean(
        runAotCheckedLenient(program(
            "    var b := new StringBuilder(\"ab\");\n"
            "    b.Mark();\n"
            "    Console.WriteLine(b.ToString());\n"
            "    Console.WriteLine(b.Twice());\n")),
        "ab!\n6\n");
}

BEZEL_TEST(stringbuilder_extension_receiver_once_7703,
           declared_member_still_answered_by_builtin_spec_26_5_rule_1) {
    // `Append` / `Length` / `ToString` are declared by stdlib §3.1 and stay
    // with the built-in: the early decline is confined to undeclared names.
    expectClean(
        runAotCheckedLenient(program(
            "    var n := new StringBuilder(C.Tag(\"r\")).Append(C.Tag(\"s\")).Length;\n"
            "    Console.WriteLine(n);\n")),
        "eval r\neval s\n2\n");
}

BEZEL_TEST(stringbuilder_extension_receiver_once_7703,
           fresh_receiver_extension_loop_100_no_use_after_free_spec_26_7) {
    // 100 evaluations: the receiver is the one value the expression produced
    // and is borrowed by the extension; generated-code ASan fails the row on a
    // use-after-free (LeakSanitizer is off, see the header).
    expectClean(
        runAotCheckedLenient(program(
            "    var tot := 0;\n"
            "    for var i in 0..<100 do\n"
            "      tot := tot + new StringBuilder(\"ab\").Append(\"c\").Twice();\n"
            "    end;\n"
            "    Console.WriteLine(tot);\n")),
        "600\n");
}
