// `[await] using var x: Object := s;` over an `Object`-declared OWNED binding
// (issues #7390, #7568; spec §28.10, §28.12, §28.3, v0.350).
//
// THE GOVERNING TEXT.
//
//   docs/Bezel_Language_Spec.md (§28.10, v0.350), *Adopting a named owned
//   binding*: an UNMARKED `using var y := x;` is a compile error "when its
//   right-hand side is a named owned binding whose declared type, nullability
//   stripped, is a class or interface type". `Object` is a class (§8.1), and
//   §28.10 carries no carve-out for it (cf. §28.14's "a class or interface
//   type other than `Object`"). The owner ruling on issue #7568, reading (A),
//   decides it: over an `Object`-declared binding in OWNED state the unmarked
//   form is the §28.10 error, with the class / interface remedies (`move s`
//   to adopt, or a `var` / `const` view).
//
//   §28.12 (v0.350): `await using var` takes §28.10's admission UNCHANGED, so
//   the await spelling and the plain spelling reach the SAME verdict.
//
// HISTORY. Issue #7390 (commit 633faf4a) fixed `await using var x: Object :=
// s;` being rejected as a suspension of the owned `s` while its `using var`
// twin compiled, by making the two spellings agree on ACCEPTANCE. The ruling
// on #7568 flips both spellings to REJECTION: rows 1, 2, 3 and 5 of the #7390
// suite pinned the accepted reading and are rewritten here to assert the
// specific diagnostic. Each also gains a `move s` twin that keeps the old
// run-time intent (the single disposal, after END) and runs under ASan/LSan.
// Row 4 (an `Object` PARAMETER, a borrow) is unchanged: it stays a pin.
//
// THE ORACLE. Each reject row pins the declaration's own "is ambiguous" text
// and its `move` remedy, not a bare compile failure. Each run row uses
// `runAotChecked` and pins the printed disposal output; the loop row counts
// disposals over 200 iterations so a doubled or suppressed disposal cannot
// hide behind a one-shot run.
//
// per §28.10, §28.12, §28.3, §8.1, v0.350

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
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

void expectRejectedAmbiguous(const JitResult& r, const char* adoptHint) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "is ambiguous"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, adoptHint));
}

void expectRunOutput(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// --- Row 1: `await using var x: Object := s;` over an owned `Object` -------

BEZEL_TEST(ownership_await_using_var_object_head_7390,
           spec_28_12_await_using_var_annotated_object_over_an_owned_binding_is_rejected) {
    // The issue's cell `object_class` / `await_using_var`: annotated head.
    // §28.12 takes §28.10's admission unchanged: the unmarked form is the error.
    expectRejectedAmbiguous(runAotChecked(asyncMain(
        "var s: Object := new Res(); await using var x: Object := s; ")),
        "write `move s` to adopt");
}

BEZEL_TEST(ownership_await_using_var_object_head_7390,
           spec_28_12_await_using_var_annotated_object_move_twin_disposes_once) {
    // The remedy: `move s` adopts. `x` owns the object; the single DTOR runs
    // when `x`'s scope ends, after END.
    expectRunOutput(runAotChecked(asyncMain(
        "var s: Object := new Res(); await using var x: Object := move s; ")),
        "END\nDTOR\n");
}

// --- Row 2: the `using var` control twin -----------------------------------

BEZEL_TEST(ownership_await_using_var_object_head_7390,
           spec_28_10_using_var_annotated_object_over_an_owned_binding_is_rejected) {
    // The plain spelling reaches the same verdict as the await one (§28.12).
    expectRejectedAmbiguous(runAotChecked(asyncMain(
        "var s: Object := new Res(); using var x: Object := s; ")),
        "write `move s` to adopt");
}

BEZEL_TEST(ownership_await_using_var_object_head_7390,
           spec_28_10_using_var_annotated_object_view_twin_disposes_once) {
    // The `var` view remedy keeps the old run-time intent: `s` keeps the
    // obligation and the single DTOR runs at its scope end, after END. (The
    // `move s` adoption of an `Object` local into a plain `using var` is not
    // pinned here: it leaks today, a separate defect reported with this commit.
    // The `await using var ... := move s` rows below pin the adoption.)
    expectRunOutput(runAotChecked(asyncMain(
        "var s: Object := new Res(); var x: Object := s; ")),
        "END\nDTOR\n");
}

// --- Row 3: the unannotated copy of an `Object` local ----------------------

BEZEL_TEST(ownership_await_using_var_object_head_7390,
           spec_28_12_await_using_var_unannotated_over_an_object_binding_is_rejected) {
    // The issue's cell `object_class` / `copy_await_source`: the unannotated
    // twin over a `var` local of declared type `Object`.
    expectRejectedAmbiguous(runAotChecked(asyncMain(
        "var src: Object := new Res(); await using var x := src; ")),
        "write `move src` to adopt");
}

BEZEL_TEST(ownership_await_using_var_object_head_7390,
           spec_28_12_await_using_var_unannotated_object_move_twin_disposes_once) {
    expectRunOutput(runAotChecked(asyncMain(
        "var src: Object := new Res(); await using var x := move src; ")),
        "END\nDTOR\n");
}

// --- Row 4: an `Object` parameter is a borrow: still a pin (unchanged) -----

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

// --- The #7390 suspend-local fix, kept pinned on a borrow-state source -------

BEZEL_TEST(ownership_await_using_var_object_head_7390,
           spec_28_12_await_using_var_over_a_borrowing_alias_is_no_suspension_of_the_owner) {
    // The surviving shape of #7390's bug: `await using var` over a BORROW-state
    // source (here a borrowing alias `v` of the owned `s`) assumes no obligation
    // and awaits nothing (§28.12), so the owned `s` is not live across an
    // await (§28.3) and `Main` is not rejected as dropping it undisposed. `s`
    // keeps the obligation: one DTOR, after END.
    expectRunOutput(runAotChecked(asyncMain(
        "var s := new Res(); var v := s; await using var x := v; ")),
        "END\nDTOR\n");
}

// --- Row 5: the loop ---------------------------------------------------------

BEZEL_TEST(ownership_await_using_var_object_head_7390,
           spec_28_12_await_using_var_object_head_in_a_loop_over_an_owned_binding_is_rejected) {
    expectRejectedAmbiguous(runAotChecked(
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
        "write `move s` to adopt");
}

BEZEL_TEST(ownership_await_using_var_object_head_7390,
           spec_28_12_await_using_var_object_head_move_twin_in_a_loop_disposes_each_owner_once) {
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
        "      await using var x: Object := move s; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(Res.n); "
        "  end Main; "
        "end Program;"),
        "200\n");
}
