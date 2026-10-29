// `throw c;` over a `using var` binding is a compile error (issue #7412; spec
// §31.1 *The throw operand*, §28.3.7, axiom A5, §28.10, §28.6, v0.350; owner
// ruling "reading 1" on the issue).
//
// THE GOVERNING TEXT.
//
//   docs/Bezel_Language_Spec.md (§28.3.7, v0.350): "A `using var` is owned but
//   is additionally bound to its scope: it cannot be moved out of, only
//   borrowed from (axiom A5). A value that must be transferred out is
//   introduced as a non-`using var` owned local instead."
//
//   docs/Bezel_Language_Spec.md (§31.1, *The throw operand*): the operand "is
//   consumed as an owned value under the ownership rules below, so a borrowed
//   ... reference is not a valid operand". `throw` is an obligation-moving
//   kill (post-state moved-from, the obligation passes to the exception
//   machinery, which disposes the exception after the catch arm): a move out of
//   the binding's scope, which A5 bars. `Dispose()` on a `using var` stays
//   admitted (§28.6): it disposes in place and moves nothing.
//
// THE BUG (#7412). The compiler accepted `using var c := new R(); throw c;`
// (the r35 reading: a site-announced kill, admitted on a pin). The owner ruled
// the executable model's reading: it is the A5 compile error, and the remedy
// is to declare the exception with a plain `var`.
//
// THE ORACLE. Each reject row pins the SPECIFIC diagnostic (it names the
// `using var`, A5 and the plain-`var` remedy), not a bare compile failure. Each
// accept row runs under ASan/LSan (`runAotChecked`) and pins the printed result
// over 200 iterations, so a doubled or a suppressed disposal of the thrown
// exception cannot hide behind a one-shot run.
//
// per §31.1, §28.3.7, §28.10, §28.6, A5, v0.350

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {

// An exception class whose destructor COUNTS, so the number of disposals of the
// thrown instance is observable.
const char* kDecls =
    "public class R: Exception "
    "  public static n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor R.n := R.n + 1; end destructor; "
    "end R; ";

std::string mainProg(const std::string& body, const std::string& tail) {
    return std::string("module M; ") + kDecls +
           "public class Program "
           "  public static method Main(): int " + body + tail +
           "  end Main; "
           "end Program;";
}

// 200 iterations of `loopBody`, then print the live count of disposals.
std::string loop200(const std::string& loopBody) {
    return mainProg(
        "    var i := 0; "
        "    while i < 200 do " + loopBody +
        "      i := i + 1; "
        "    end; ",
        "    Console.WriteLine(R.n); return 0; ");
}

void expectRejectedUsingVarThrow(const JitResult& r, const char* name) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("cannot throw `using var` `") + name + "`").c_str()));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "axiom A5"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "plain `var`"));
}

void expectRunOutput(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// --- REJECT: `throw` of a `using var` ----------------------------------------

BEZEL_TEST(ownership_throw_using_var_operand_7412,
           spec_31_1_throw_of_a_using_var_is_rejected) {
    // The issue's repro: `using var c := new R(); throw c;` inside a `try`.
    expectRejectedUsingVarThrow(runAotChecked(mainProg(
        "    try "
        "      using var c := new R(); "
        "      throw c; "
        "    catch var e: R => "
        "      Console.WriteLine(\"caught\"); "
        "    end; ",
        "    Console.WriteLine(\"END\"); return 0; ")), "c");
}

BEZEL_TEST(ownership_throw_using_var_operand_7412,
           spec_31_1_throw_of_a_null_forgiving_using_var_is_rejected) {
    // `throw c!;` reads the same binding (§17.8).
    expectRejectedUsingVarThrow(runAotChecked(mainProg(
        "    try "
        "      using var c := new R(); "
        "      throw c!; "
        "    catch var e: R => "
        "      Console.WriteLine(\"caught\"); "
        "    end; ",
        "    Console.WriteLine(\"END\"); return 0; ")), "c");
}

BEZEL_TEST(ownership_throw_using_var_operand_7412,
           spec_31_1_throw_of_a_using_var_in_expression_position_is_rejected) {
    // `n ?? throw c` is the expression-position throw (§17.12): the same
    // operand, the same rule.
    expectRejectedUsingVarThrow(runAotChecked(mainProg(
        "    var n: int? := null; "
        "    try "
        "      using var c := new R(); "
        "      var x := n ?? throw c; "
        "    catch var e: R => "
        "      Console.WriteLine(\"caught\"); "
        "    end; ",
        "    Console.WriteLine(\"END\"); return 0; ")), "c");
}

BEZEL_TEST(ownership_throw_using_var_operand_7412,
           spec_31_1_throw_of_a_using_var_outside_any_try_is_rejected) {
    // Not an artifact of the `try`: a method that throws its own pin.
    expectRejectedUsingVarThrow(runAotChecked(
        std::string("module M; ") + kDecls +
        "public class Program "
        "  public static method Boom() "
        "    using var c := new R(); "
        "    throw c; "
        "  end Boom; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"END\"); return 0; "
        "  end Main; "
        "end Program;"), "c");
}

// --- ACCEPT: the remedy, and what A5 does not bar ----------------------------

BEZEL_TEST(ownership_throw_using_var_operand_7412,
           spec_31_1_throw_of_a_plain_var_is_consumed_and_disposed_once_per_throw) {
    // The remedy: a `var`-bound owned exception is consumed by `throw` and
    // disposed by the runtime after the catch arm: exactly one disposal per
    // throw, 200 throws.
    expectRunOutput(runAotChecked(loop200(
        "      try "
        "        var c := new R(); "
        "        throw c; "
        "      catch var e: R => Console.Write(\"\"); "
        "      end; ")), "200\n");
}

BEZEL_TEST(ownership_throw_using_var_operand_7412,
           spec_31_1_throw_of_a_null_forgiving_plain_var_disposes_once_per_throw) {
    expectRunOutput(runAotChecked(loop200(
        "      try "
        "        var c := new R(); "
        "        throw c!; "
        "      catch var e: R => Console.Write(\"\"); "
        "      end; ")), "200\n");
}

BEZEL_TEST(ownership_throw_using_var_operand_7412,
           spec_31_1_throw_of_a_plain_var_in_expression_position_disposes_once_per_throw) {
    expectRunOutput(runAotChecked(mainProg(
        "    var n: int? := null; "
        "    var i := 0; "
        "    while i < 200 do "
        "      try "
        "        var c := new R(); "
        "        var x := n ?? throw c; "
        "      catch var e: R => Console.Write(\"\"); "
        "      end; "
        "      i := i + 1; "
        "    end; ",
        "    Console.WriteLine(R.n); return 0; ")), "200\n");
}

BEZEL_TEST(ownership_throw_using_var_operand_7412,
           spec_28_6_explicit_dispose_on_a_using_var_stays_admitted) {
    // `Dispose()` disposes in place and moves nothing (§28.6): the pin's scope
    // end then finds an explicitly-disposed binding. One disposal per
    // iteration, 200 iterations.
    expectRunOutput(runAotChecked(loop200(
        "      do "
        "        using var c := new R(); "
        "        c.Dispose(); "
        "      end; ")), "200\n");
}

BEZEL_TEST(ownership_throw_using_var_operand_7412,
           spec_31_1_throw_of_a_fresh_exception_beside_a_using_var_is_unaffected) {
    // A `using var` that is NOT the operand: the thrown value is fresh, and the
    // pin is disposed by the unwind, once. Two disposals per iteration (the
    // pin and the thrown exception), 200 iterations.
    expectRunOutput(runAotChecked(loop200(
        "      try "
        "        using var keep := new R(); "
        "        throw new R(); "
        "      catch var e: R => Console.Write(\"\"); "
        "      end; ")), "400\n");
}
