// fix #8181 (stdlib §5.1, spec §14.2 *Interface conformance* rule 6, §26.5.1,
// v0.355): the leak-checked (runAotChecked: generated-code ASan + LeakSanitizer)
// rows for a §5 LINQ call inside a GENERIC body. An unconstrained `T` satisfies
// no `where T: …` clause (§14.2 rule 6: "T is a type parameter whose constraints
// guarantee conformance to I"), so on an `Array<T>` receiver the §5 `Sum()` is
// not an applicable candidate and a same-named `extension Object` member answers,
// at every instantiation; a constrained `T` keeps the §5 overload.
//
// MEASURED ON THE PARENT (r30-c1 @ 737e6d0a, plus the #8180 commit): the
// T = string rows aborted in `verifyModule` ("IR verification failed (malformed
// IR emitted)"); the T = int row printed 5 per call (BEHAVIOUR FLIP, see the
// sema file's header); the constrained controls printed the same output.
//
// The rows are LOOPED (100 iterations) so a per-call leak cannot hide. The
// diagnostic-free sema rows live in
// tests/sema/linq_generic_body_where_applicability_8181_test.cpp.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string looped(const std::string& decls, const std::string& loopBody) {
    return "module M;\n"
           "extension Object\n"
           "  public method Sum(): int => 4242;\n"
           "end;\n" + decls +
           "class D\n"
           "  public static method Main(): int\n"
           "    var i := 0;\n"
           "    var t := 0;\n"
           "    while i < 100 do\n" +
           loopBody +
           "      i := i + 1;\n"
           "    end;\n"
           "    Console.WriteLine(t);\n"
           "    return 0;\n"
           "  end Main;\n"
           "end D;\n";
}

}  // namespace

// The issue's repro (T = string), its generic-class and `extension Array<T>`
// twins. 100 x 3 x 4242 = 1272600. PARENT: malformed IR.
BEZEL_TEST(codegen_linq_generic_body_where_applicability_8181,
           generic_sum_at_string_answers_the_extension_without_leaking) {
    JitResult r = runAotChecked(looped(
        "class C\n"
        "  public static method G<T>(a: Array<T>): int => a.Sum();\n"
        "end C;\n"
        "class E<T>\n"
        "  public method G(a: Array<T>): int => a.Sum();\n"
        "end E;\n"
        "extension Array<T>\n"
        "  public method Total(): int => this.Sum();\n"
        "end;\n",
        "      var a := new Array<string>();\n"
        "      a.Add(\"x\");\n"
        "      var e := new E<string>();\n"
        "      t := t + C.G(a) + e.G(a) + a.Total();\n"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1272600\n");
}

// BEHAVIOUR FLIP: the same unconstrained body at T = int. 100 x 4242.
// PARENT: 500 (the §5 `Sum()`, 5 per call).
BEZEL_TEST(codegen_linq_generic_body_where_applicability_8181,
           generic_sum_at_int_answers_the_extension_without_leaking) {
    JitResult r = runAotChecked(looped(
        "class C\n"
        "  public static method G<T>(a: Array<T>): int => a.Sum();\n"
        "end C;\n",
        "      var a := new Array<int>();\n"
        "      a.Add(5);\n"
        "      t := t + C.G(a);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "424200\n");
}

// CONTROL (silent-acceptance mirror): an `INumber<T>`-constrained body and a
// concrete `Array<int>` keep the §5 `Sum()`. 100 x (11 + 11) = 2200.
// PARENT: the same output.
BEZEL_TEST(codegen_linq_generic_body_where_applicability_8181,
           constrained_and_concrete_sum_keep_the_builtin_without_leaking) {
    JitResult r = runAotChecked(looped(
        "class C\n"
        "  public static method G<T>(a: Array<T>): T where T: INumber<T> =>"
        " a.Sum();\n"
        "end C;\n",
        "      var a := new Array<int>();\n"
        "      a.Add(5);\n"
        "      a.Add(6);\n"
        "      t := t + C.G(a) + a.Sum();\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2200\n");
}
