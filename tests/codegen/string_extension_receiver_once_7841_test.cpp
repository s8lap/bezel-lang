// Regression tests for #7841 -- a user extension call on a `string`
// receiver that is a CALL RESULT evaluated the receiver expression TWICE
// (`Make.Str(1).Len2()` called `Str` twice): the built-in string arm of
// tryLowerCollectionInstanceCall lowered the receiver, declined the member it
// does not declare, and the extension lowering lowered the receiver again.
//
// Language spec §22.5 (v0.355): an operand is evaluated once, in written
// order. §26.7 (v0.355): the receiver is a borrow of the one value its
// expression denoted when it was evaluated. §26.5 rules 1-2: a name the
// receiver's declared type declares commits to the built-in; any other name
// selects the extension.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

void expectClean(const JitResult& r, const char* out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

const char* kPrelude =
    "module M;\n"
    "public class Make\n"
    "  public static Calls: int := 0;\n"
    "  public static method Str(n: int): string\n"
    "    Make.Calls := Make.Calls + 1;\n"
    "    return \"s\" + n.ToString();\n"
    "  end Str;\n"
    "end Make;\n"
    "public extension string\n"
    "  public method Len2(): int => this.Length;\n"
    "  public method Join(o: string): string => this + o;\n"
    "end;\n"
    "public class Program\n"
    "  public static method Main(): int\n";

std::string program(const std::string& body) {
    return std::string(kPrelude) + body +
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

}  // namespace

BEZEL_TEST(string_extension_receiver_once_7841,
           call_result_receiver_evaluated_once_spec_22_5_26_7) {
    expectClean(
        runAotChecked(program(
            "    var t := 0;\n"
            "    t := t + Make.Str(1).Len2();\n"
            "    Console.WriteLine($\"{t} {Make.Calls}\");\n")),
        "2 1\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           call_result_receiver_with_argument_evaluated_once_spec_22_5) {
    expectClean(
        runAotChecked(program(
            "    var s := Make.Str(1).Join(\"x\");\n"
            "    Console.WriteLine($\"{s} {Make.Calls}\");\n")),
        "s1x 1\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           owned_string_receiver_loop_no_leak_spec_26_7) {
    expectClean(
        runAotChecked(program(
            "    var t := 0;\n"
            "    for var i in 0..99 do\n"
            "      t := t + Make.Str(i).Len2();\n"
            "    end;\n"
            "    Console.WriteLine($\"{t} {Make.Calls}\");\n")),
        "290 100\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           builtin_member_still_answers_declared_name_spec_26_5) {
    expectClean(
        runAotChecked(program(
            "    var n := Make.Str(1).Length;\n"
            "    Console.WriteLine($\"{n} {Make.Calls}\");\n")),
        "2 1\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           extension_returning_receiver_no_double_free_spec_26_7) {
    // The result aliases the receiver: the receiver temporary's reference and
    // the result's reference are released independently (ASan: no UAF, no leak).
    expectClean(
        runAotChecked(
            "module M;\n"
            "public class Make\n"
            "  public static method Str(n: int): string\n"
            "    return \"s\" + n.ToString();\n"
            "  end Str;\n"
            "end Make;\n"
            "public extension string\n"
            "  public method Self(): string => this;\n"
            "end;\n"
            "public class Program\n"
            "  public static method Main(): int\n"
            "    var t := 0;\n"
            "    for var i in 0..99 do\n"
            "      var r := Make.Str(i).Self();\n"
            "      t := t + r.Length;\n"
            "    end;\n"
            "    Console.WriteLine(t);\n"
            "    return 0;\n"
            "  end Main;\n"
            "end Program;\n"),
        "290\n");
}

// The fix-forward rows (review of a850ccae, probes p07 / p02 / p06): an
// extension whose result EMBEDS `this` unretained (a record holding the
// receiver string). §26.7 (v0.355): "The receiver is always a borrow". The
// receiver temporary must outlive every read through the result -- an
// immediate post-call release was a heap-use-after-free -- and must still be
// released exactly once (ASan: no UAF, no leak).
namespace {

const char* kWrapPrelude =
    "module M;\n"
    "public record Named(Tag: string, N: int);\n"
    "public extension string\n"
    "  public method Wrap(): Named => new Named(this, 1);\n"
    "end;\n"
    "public class Make\n"
    "  public static method Str(n: int): string => \"s\" + n.ToString();\n"
    "end Make;\n"
    "public class Program\n"
    "  public static method Main(): int\n";

std::string wrapProgram(const std::string& body) {
    return std::string(kWrapPrelude) + body +
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

}  // namespace

BEZEL_TEST(string_extension_receiver_once_7841,
           wrap_result_embeds_receiver_one_shot_no_uaf_spec_26_7) {
    // p07: base printed "a1"; a850ccae crashed in bz_string_to_utf8z.
    expectClean(
        runAotChecked(wrapProgram(
            "    var n := (\"a\" + 1.ToString()).Wrap();\n"
            "    Console.WriteLine(n.Tag);\n")),
        "a1\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           wrap_result_bound_to_local_loop_no_uaf_no_leak_spec_26_7) {
    // p02: the record local's own scope-end string-field release owns the
    // receiver (§28.9), exactly as for the static twin `Make.Wrap(Make.Str(i))`.
    expectClean(
        runAotChecked(wrapProgram(
            "    var t := 0;\n"
            "    for var i in 0..199 do\n"
            "      var n := Make.Str(i).Wrap();\n"
            "      t := t + n.Tag.Length;\n"
            "    end;\n"
            "    Console.WriteLine(t);\n")),
        "690\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           wrap_result_field_read_inline_loop_no_uaf_no_leak_spec_26_7) {
    // p06: the result is read within the full expression; the receiver is
    // released at its end, after the read (§28.3 K8.1).
    expectClean(
        runAotChecked(wrapProgram(
            "    var t := 0;\n"
            "    for var i in 0..199 do\n"
            "      t := t + Make.Str(i).Wrap().Tag.Length;\n"
            "    end;\n"
            "    Console.WriteLine(t);\n")),
        "690\n");
}
