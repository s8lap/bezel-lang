// A `using var` over a field or property read of an UNBOUND OWNED TEMPORARY
// is a COMPILE ERROR (issue #7687; spec K8.4, §28.10, §28.3, §28.12, v0.350).
//
// THE GOVERNING TEXT.
//
//   K8.4 (v0.350): "A temporary is an admissible borrow source only within its
//   boundary; a borrow or frame that would escape it is **rejected, never
//   lifetime-extended** … The escapes rejected are every escape K2.3 can
//   compare — among them a binding, store, capture, `return` or `yield
//   return` of the borrow".
//
//   §28.10 (v0.350), *`using var` initialized from a borrow*: a field read is
//   a borrow, and the `using var` over it "does **not** assume a disposal
//   obligation" — it pins the borrow's source. When that source is a
//   temporary, the pin would outlive it: the K8 boundary disposes the
//   temporary at the end of the declaration's statement.
//
//   §28.3's own worked example is the same shape through a call:
//   `var t := new Shelf().Top();  // error (K8.4): `t` would borrow from a
//   temporary disposed at this statement's end`; §30.5 names "a `using var`
//   bind" among the escapes K8.4 rejects.
//
// THE BUG (#7687). Every shape below was ACCEPTED. Before staging 106e3167
// the temporary receiver leaked (it was never disposed: count 100 plus LSan
// in the issue's m04 probes); since 106e3167 the payload leaked too (count 0
// plus LSan), because the pin flag is set for a field read whatever its
// receiver. The rejection removes the program, and with it both leaks.
//
// NOT covered here: the plain `var x := MkH().F;` / `const` twins and the
// borrow-returning CALL spelling (`var t := new Shelf().Top();`), which are
// open issues #7402, #7445 and #7627 and are deliberately unchanged.
//
// THE ORACLE. Each reject row pins the specific diagnostic. The control rows
// run under ASan/LSan (`runAotChecked`) and loop 100 times, with a static
// destructor counter showing each object disposed exactly once.
//
// per K8.4, §28.10, §28.3, §28.12, v0.350

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {

// `Res.Count` counts disposals. A live `H` holds three owned `Res`: in the
// class field `C`, the owning choice `F`, and slot 0 of the owning inline
// array `A` (the nullable choice `G` starts empty).
const char* kDecls =
    "public class Res "
    "  public static Count: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.Count := Res.Count + 1; end destructor; "
    "end Res; "
    "public choice OC Text(owned r: Res), Empty; end OC; "
    "public class H "
    "  public owned C: Res := new Res(); "
    "  public owned F: OC := OC.Text(new Res()); "
    "  public owned G: OC? := null; "
    "  public A: [2 owned]Res? := [new Res(), null]; "
    "  public N: int := 7; "
    "  public S: string := \"s\"; "
    "  public property PF: OC => F; "
    "  public method Self(): H => this; "
    "  public constructor() end constructor; "
    "end H; "
    "public class Outer "
    "  public owned B: H := new H(); "
    "  public constructor() end constructor; "
    "end Outer; "
    "public class Make "
    "  public static method MkH(): owned H return new H(); end MkH; "
    "  public static method MkHN(): owned H? return new H(); end MkHN; "
    "  public static method MkO(): owned Outer return new Outer(); end MkO; "
    "end Make; ";

std::string mainProg(const std::string& body) {
    return std::string("module M; ") + kDecls +
           "public class Program "
           "  public static method Main(): int " + body +
           "    return 0; "
           "  end Main; "
           "end Program;";
}

void expectRejectedTemporary(const JitResult& r) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "escapes the temporary's full-expression boundary"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "(K8.4"));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "bind the receiver to a name first"));
}

void expectRunOutput(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// --- REJECT: a pin of a temporary's field (K8.4) -----------------------------

BEZEL_TEST(ownership_using_var_field_of_temporary_7687,
           spec_k8_4_using_var_over_a_move_only_field_of_an_owned_call_is_rejected) {
    // The issue's m04 / m04b shapes.
    expectRejectedTemporary(
        runAotChecked(mainProg("do using var x := Make.MkH().F; end; ")));
    expectRejectedTemporary(
        runAotChecked(mainProg("do using var y := Make.MkH().A; end; ")));
}

BEZEL_TEST(ownership_using_var_field_of_temporary_7687,
           spec_k8_4_using_var_over_a_field_of_a_new_expression_is_rejected) {
    // The issue's m04c shape (the parentheses introduce no expression).
    expectRejectedTemporary(
        runAotChecked(mainProg("do using var x := (new H()).F; end; ")));
}

BEZEL_TEST(ownership_using_var_field_of_temporary_7687,
           spec_k8_4_using_var_over_a_class_field_of_a_temporary_is_rejected) {
    expectRejectedTemporary(
        runAotChecked(mainProg("do using var c := Make.MkH().C; end; ")));
}

BEZEL_TEST(ownership_using_var_field_of_temporary_7687,
           spec_k8_4_the_bang_null_conditional_and_property_spellings_are_rejected) {
    // §17.8 (`!` classifies as the bare read), §7.5 (`?.`), §28.6 (a getter
    // lends), and a nested field path rooted at the temporary.
    expectRejectedTemporary(
        runAotChecked(mainProg("do using var g := Make.MkH().G!; end; ")));
    expectRejectedTemporary(
        runAotChecked(mainProg("do using var f := Make.MkHN()?.F; end; ")));
    expectRejectedTemporary(
        runAotChecked(mainProg("do using var p := Make.MkH().PF; end; ")));
    expectRejectedTemporary(
        runAotChecked(mainProg("do using var b := Make.MkO().B.F; end; ")));
}

BEZEL_TEST(ownership_using_var_field_of_temporary_7687,
           spec_k8_4_a_meet_arm_over_a_temporarys_field_is_rejected) {
    // §28.10: a meet of field reads is a pin of the union of sources; one of
    // them is a temporary.
    expectRejectedTemporary(runAotChecked(mainProg(
        "var h := new H(); "
        "do using var x := (if h.N = 7 then Make.MkH().F else h.F); end; ")));
}

BEZEL_TEST(ownership_using_var_field_of_temporary_7687,
           spec_k8_4_anonymous_and_await_using_spellings_are_rejected) {
    // §28.12: `await using var` takes §28.10's admission unchanged.
    expectRejectedTemporary(
        runAotChecked(mainProg("do using var _ := Make.MkH().F; end; ")));
    expectRejectedTemporary(runAotChecked(
        std::string("module M; ") + kDecls +
        "public class Program "
        "  public static async method Take(): Task "
        "    await using var x := Make.MkH().C; "
        "  end Take; "
        "  public static async method Main(): Task await Take(); end Main; "
        "end Program;"));
}

// --- controls ------------------------------------------------------------------

BEZEL_TEST(ownership_using_var_field_of_temporary_7687,
           spec_28_10_control_bind_the_receiver_first_then_pin_its_field) {
    // The diagnostic's remedy: the named receiver owns the payloads, the pin
    // disposes nothing, and each `Res` is disposed once when `h` dies.
    expectRunOutput(
        runAotChecked(mainProg(
            "var early := 0; "
            "for var i in 0..<100 do "
            "  do using var h := Make.MkH(); "
            "    do using var x := h.F; using var y := h.A; using var c := h.C; end; "
            "    if Res.Count <> 3 * i then early := early + 1; end if; "
            "  end; "
            "end; "
            "Console.WriteLine($\"{early} {Res.Count}\"); ")),
        "0 300\n");
}

BEZEL_TEST(ownership_using_var_field_of_temporary_7687,
           spec_28_10_control_a_value_field_of_a_temporary_is_a_copy) {
    // A value-typed field read is no borrow (§28.10: "`using var` accepts a
    // value-type or runtime-managed right-hand side"), so nothing escapes and
    // the rejection must not reach it; the temporary is disposed at its
    // boundary. (The `string`-typed twin is not pinned here: it leaks the
    // temporary receiver on the base and on this fix alike, under `using var`
    // and `const` both, a separate defect reported with this change.)
    expectRunOutput(
        runAotChecked(mainProg(
            "var t := 0; "
            "for var i in 0..<100 do "
            "  do using var n := Make.MkH().N; t := t + n; end; "
            "end; "
            "Console.WriteLine($\"{t} {Res.Count}\"); ")),
        "700 300\n");
}

BEZEL_TEST(ownership_using_var_field_of_temporary_7687,
           spec_28_10_control_a_field_of_a_borrow_returning_call_is_a_pin) {
    // `h.Self()` is a call without an `owned` return — a borrow of `h`, not a
    // temporary — so pinning its field is §28.10's ordinary pin.
    expectRunOutput(
        runAotChecked(mainProg(
            "var early := 0; "
            "for var i in 0..<100 do "
            "  do var h := new H(); "
            "    do using var x := h.Self().F; end; "
            "    if Res.Count <> 3 * i then early := early + 1; end if; "
            "  end; "
            "end; "
            "Console.WriteLine($\"{early} {Res.Count}\"); ")),
        "0 300\n");
}
