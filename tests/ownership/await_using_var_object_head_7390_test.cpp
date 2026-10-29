// `await using var x: Object := s;` over an `Object`-declared binding must be
// accepted exactly as its `using var` twin is (issue #7390; spec §28.12,
// §28.10, §28.3, v0.350).
//
// THE GOVERNING TEXT.
//
//   docs/Bezel_Language_Spec.md (§28.12, v0.350): `await using var` takes
//   §28.10's admission UNCHANGED: over a right-hand side that §28.10 admits as
//   a borrow, "nothing is disposed or awaited". So the await spelling and the
//   plain spelling must reach the SAME verdict on the same right-hand side.
//
//   docs/Bezel_Language_Spec.md (§28.3, v0.350), the suspend-local rule: an
//   owned local that is live across a suspension point is reported; a
//   declaration that is a borrow-pin disposes nothing and awaits nothing, so
//   it is not a suspension point of an owned value.
//
// THE BUG (#7390). The plain `using var x: Object := s;` compiled and ran
// (printing the single disposal), while `await using var x: Object := s;` was
// rejected: the suspend-local pass treated the `await using` declaration as an
// await site of the enclosing block even though the declaration was a
// borrow-pin and awaits nothing.
//
// THE ORACLE. Every row runs under ASan/LSan (`runAotChecked`) and pins the
// printed disposal output: the single DTOR is the OWNER's (`s`), after END, in
// BOTH spellings. The loop row counts disposals over 200 iterations so a
// doubled or a suppressed disposal cannot hide behind a one-shot run.
//
// Not decided here: whether an `Object`-declared binding is a "class or
// interface type" for §28.10's *Adopting a named owned binding* (the grid's
// recorded open question 1, `OPEN_QUESTION = {'object_class'}`). These rows pin
// only that the two spellings AGREE, which §28.12's "unchanged" requires.
//
// per §28.12, §28.10, §28.3, v0.350

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

const char* kRes =
    "public class Res "
    "  public constructor() end constructor; "
    "  destructor Console.WriteLine(\"DTOR\"); end destructor; "
    "end Res; ";

std::string asyncMain(const std::string& body) {
    return std::string("module M; ") + kRes +
           "public class Program "
           "  public static async method Main(): Task " + body +
           "    Console.WriteLine(\"END\"); "
           "  end Main; "
           "end Program;";
}

void expectRunOutput(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

BEZEL_TEST(ownership_await_using_var_object_head_7390,
           spec_28_12_await_using_var_annotated_object_matches_the_using_var_twin) {
    // The issue's cell `object_class` / `await_using_var`: annotated head.
    expectRunOutput(runAotChecked(asyncMain(
        "var s: Object := new Res(); await using var x: Object := s; ")),
        "END\nDTOR\n");
}

BEZEL_TEST(ownership_await_using_var_object_head_7390,
           spec_28_10_using_var_annotated_object_is_the_control_twin) {
    // The twin the issue says compiles: same program, `using var`.
    expectRunOutput(runAotChecked(asyncMain(
        "var s: Object := new Res(); using var x: Object := s; ")),
        "END\nDTOR\n");
}

BEZEL_TEST(ownership_await_using_var_object_head_7390,
           spec_28_12_await_using_var_unannotated_over_an_object_binding) {
    // The issue's cell `object_class` / `copy_await_source`: the copy of a
    // `var` local source, unannotated.
    expectRunOutput(runAotChecked(asyncMain(
        "var src: Object := new Res(); await using var x := src; ")),
        "END\nDTOR\n");
}

BEZEL_TEST(ownership_await_using_var_object_head_7390,
           spec_28_12_await_using_var_over_an_object_parameter) {
    // A plain `Object` parameter is a borrow: the caller disposes.
    expectRunOutput(runAotChecked(
        std::string("module M; ") + kRes +
        "public class Util "
        "  public static async method Look(src: Object): Task "
        "    await using var x: Object := src; "
        "  end Look; "
        "end Util; "
        "public class Program "
        "  public static async method Main(): Task "
        "    using var s: Object := new Res(); "
        "    await Util.Look(s); "
        "    Console.WriteLine(\"END\"); "
        "  end Main; "
        "end Program;"),
        "END\nDTOR\n");
}

BEZEL_TEST(ownership_await_using_var_object_head_7390,
           spec_28_12_await_using_var_object_head_in_a_loop_disposes_each_owner_once) {
    // 200 iterations, one `Res` each: exactly 200 disposals, one per
    // construction (a one-shot repro is vacuous for leaks).
    expectRunOutput(runAotChecked(
        "module M; "
        "public class Res "
        "  public static n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor Res.n := Res.n + 1; end destructor; "
        "end Res; "
        "public class Program "
        "  public static async method Main(): Task "
        "    var i := 0; "
        "    while i < 200 do "
        "      var s: Object := new Res(); "
        "      await using var x: Object := s; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(Res.n); "
        "  end Main; "
        "end Program;"),
        "200\n");
}
