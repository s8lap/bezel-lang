// A `using var x := h.F;` over a MOVE-ONLY field (an owning-classified choice,
// an owning inline array) is a borrow-pin that disposes NOTHING (issue #7480;
// spec §28.10 *`using var` initialized from a borrow*, §28.12, v0.350).
//
// THE GOVERNING TEXT.
//
//   docs/Bezel_Language_Spec.md (§28.10, v0.350), *`using var` initialized
//   from a borrow*: "When the right-hand side of a `using var` declaration is
//   a borrow rather than an owned value (a method call without an `owned`
//   return …, an indexer access, a field read, or an unmarked read of a
//   **borrow-state** named binding …), the `using var` does **not** assume a
//   disposal obligation …: at scope end, no `Dispose()` is called, because
//   the value is not owned by this scope."
//
//   §28.12 (v0.350): "`await using var` takes §28.10's admission unchanged
//   … Over a borrow right-hand side (a call without an `owned` return, a
//   field, an indexer, a borrow-state binding) the binding assumes no
//   obligation, so scope exit disposes nothing and awaits nothing".
//
//   The same §28.10 paragraph keeps the move-only LOCAL on the other side:
//   "an unmarked read of a **move-only value** binding moves instead and is
//   an owned right-hand side" — so the control rows below pin that a
//   `using var` over a move-only local (marked or not) still disposes.
//
// THE BUG (#7480). The ownership pass classified the field read as a pin, but
// codegen's move-only local registrations (registerOwningChoiceDisposal,
// registerInlineOwnedArrayDisposal) bypassed the using-var chain that reads
// that verdict, so `x` disposed the field's payload at its scope end and the
// holder disposed it again at its own teardown: a heap-use-after-free.
//
// NOT covered here: the plain `var x := h.F;` twin, which is open
// spec-question issue #7612 and is deliberately unchanged.
//
// THE ORACLE. Every row runs under ASan/LSan (`runAotChecked`) and loops the
// shape 100 times. A static counter in the destructor counts disposals; each
// iteration checks that NO disposal happened before the holder died (a pin
// that disposed would bump the counter early), and the final count shows each
// object disposed exactly once (a doubled disposal is also an ASan error).
//
// per §28.10, §28.12, §15.3, v0.350

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// `Res.Count` counts disposals. `H2` holds two owned `Res`: one in the owning
// choice field `F`, one in slot 0 of the owning inline array field `A`.
const char* kDecls =
    "public class Res "
    "  public static Count: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.Count := Res.Count + 1; end destructor; "
    "end Res; "
    "public choice OC Text(owned r: Res), Empty; end OC; "
    "public class H2 "
    "  public owned F: OC := OC.Text(new Res()); "
    "  public A: [2 owned]Res? := [new Res(), null]; "
    "  public constructor() end constructor; "
    "  public method PinThis() using var x := this.F; using var y := this.A; end PinThis; "
    "  public method PinBare() using var x := F; using var y := A; end PinBare; "
    "end H2; "
    "public class Outer "
    "  public owned B: H2 := new H2(); "
    "  public constructor() end constructor; "
    "end Outer; ";

// 100 iterations of `iterBody`; after each, `Res.Count` must equal
// `perIter * (i + 1)` minus whatever the holder has not yet disposed, which the
// caller states as `expectAfter` (an expression over `i`).
std::string loopProg(const std::string& members, const std::string& iterBody,
                     const std::string& expectAfter) {
    return std::string("module M; ") + kDecls +
           "public class Program " + members +
           "  public static method Main(): int "
           "    var early := 0; "
           "    for var i in 0..<100 do "
           "      " + iterBody +
           "      if Res.Count <> " + expectAfter +
           " then early := early + 1; end if; "
           "    end; "
           "    Console.WriteLine($\"{early} {Res.Count}\"); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

void expectRunOutput(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// --- the pin: a field read disposes nothing (§28.10) ------------------------

BEZEL_TEST(ownership_using_var_field_read_pin_7480,
           spec_28_10_using_var_over_a_move_only_field_through_a_borrowed_param_is_a_pin) {
    // The issue's own shape: `Take(h: H2)` pins `h.F` through a plain
    // (borrowed) parameter. Inside the do-block the holder is alive, so the
    // pin must not have disposed anything: the count after `Take` returns is
    // the previous iterations' 2 per holder.
    expectRunOutput(
        runAotChecked(loopProg(
            "public static method Take(h: H2) "
            "  using var x := h.F; using var y := h.A; "
            "end Take; ",
            "do var h := new H2(); Take(h); "
            "  if Res.Count <> 2 * i then early := early + 1; end if; end; ",
            "2 * (i + 1)")),
        "0 200\n");
}

BEZEL_TEST(ownership_using_var_field_read_pin_7480,
           spec_28_10_using_var_over_a_move_only_field_of_a_local_is_a_pin) {
    expectRunOutput(
        runAotChecked(loopProg(
            "",
            "do var h := new H2(); "
            "  do using var x := h.F; using var y := h.A; end; "
            "  if Res.Count <> 2 * i then early := early + 1; end if; end; ",
            "2 * (i + 1)")),
        "0 200\n");
}

BEZEL_TEST(ownership_using_var_field_read_pin_7480,
           spec_28_10_using_var_over_this_dot_move_only_field_is_a_pin) {
    expectRunOutput(
        runAotChecked(loopProg(
            "",
            "do var h := new H2(); h.PinThis(); "
            "  if Res.Count <> 2 * i then early := early + 1; end if; end; ",
            "2 * (i + 1)")),
        "0 200\n");
}

BEZEL_TEST(ownership_using_var_field_read_pin_7480,
           spec_28_10_using_var_over_an_implicit_this_move_only_field_is_a_pin) {
    // §28.10: "A bare identifier that resolves to a field — an implicit-`this`
    // read … — is a field read."
    expectRunOutput(
        runAotChecked(loopProg(
            "",
            "do var h := new H2(); h.PinBare(); "
            "  if Res.Count <> 2 * i then early := early + 1; end if; end; ",
            "2 * (i + 1)")),
        "0 200\n");
}

BEZEL_TEST(ownership_using_var_field_read_pin_7480,
           spec_28_10_using_var_over_a_nested_move_only_field_path_is_a_pin) {
    expectRunOutput(
        runAotChecked(loopProg(
            "",
            "do var o := new Outer(); "
            "  do using var x := o.B.F; using var y := o.B.A; end; "
            "  if Res.Count <> 2 * i then early := early + 1; end if; end; ",
            "2 * (i + 1)")),
        "0 200\n");
}

BEZEL_TEST(ownership_using_var_field_read_pin_7480,
           spec_28_10_using_var_field_pin_disposes_nothing_on_the_exception_edge) {
    // Every exit path (§28.10): the throw edge out of the pin's scope must
    // not dispose the borrowed payload either.
    expectRunOutput(
        runAotChecked(loopProg(
            "public static method Take(h: H2) "
            "  using var x := h.F; using var y := h.A; "
            "  throw new InvalidOperationException(\"boom\"); "
            "end Take; ",
            "do var h := new H2(); "
            "  try Take(h); "
            "  catch var e: InvalidOperationException => Console.Write(\"\") "
            "  end try; "
            "  if Res.Count <> 2 * i then early := early + 1; end if; end; ",
            "2 * (i + 1)")),
        "0 200\n");
}

BEZEL_TEST(ownership_using_var_field_read_pin_7480,
           spec_28_12_await_using_var_over_a_move_only_field_is_a_pin) {
    // §28.12: over a field right-hand side "scope exit disposes nothing and
    // awaits nothing".
    expectRunOutput(
        runAotChecked(
            std::string("module M; ") + kDecls +
            "public class Program "
            "  public static async method Take(h: H2): Task "
            "    await using var x := h.F; await using var y := h.A; "
            "  end Take; "
            "  public static async method Main(): Task "
            "    var early := 0; "
            "    for var i in 0..<100 do "
            "      do using var h := new H2(); await Take(h); "
            "        if Res.Count <> 2 * i then early := early + 1; end if; "
            "      end; "
            "      if Res.Count <> 2 * (i + 1) then early := early + 1; end if; "
            "    end; "
            "    Console.WriteLine($\"{early} {Res.Count}\"); "
            "  end Main; "
            "end Program;"),
        "0 200\n");
}

// --- controls: an owned move-only right-hand side still disposes ------------

BEZEL_TEST(ownership_using_var_field_read_pin_7480,
           spec_28_10_control_using_var_over_a_moved_move_only_local_disposes) {
    // `using var x := move c;` adopts (§28.10 *Adopting a named owned
    // binding*): the payload is disposed at the do-block's end, once.
    expectRunOutput(
        runAotChecked(loopProg(
            "",
            "do var c := OC.Text(new Res()); using var x := move c; end; ",
            "i + 1")),
        "0 100\n");
}

BEZEL_TEST(ownership_using_var_field_read_pin_7480,
           spec_28_10_control_using_var_over_an_unmarked_move_only_local_disposes) {
    // "an unmarked read of a **move-only value** binding moves instead and is
    // an owned right-hand side": the field-read pin must not swallow this one
    // (the ownership pass sets the same borrow flag over a bare local read).
    expectRunOutput(
        runAotChecked(loopProg(
            "",
            "do var c := OC.Text(new Res()); using var x := c; end; ",
            "i + 1")),
        "0 100\n");
}

BEZEL_TEST(ownership_using_var_field_read_pin_7480,
           spec_28_10_control_using_var_over_a_move_only_array_local_disposes) {
    expectRunOutput(
        runAotChecked(loopProg(
            "",
            "do var c: [2 owned]Res? := [new Res(), null]; "
            "  using var x := move c; end; "
            "do var d: [2 owned]Res? := [new Res(), null]; "
            "  using var y := d; end; ",
            "2 * (i + 1)")),
        "0 200\n");
}

BEZEL_TEST(ownership_using_var_field_read_pin_7480,
           spec_28_10_control_using_var_over_a_fresh_owning_choice_disposes) {
    expectRunOutput(
        runAotChecked(loopProg(
            "",
            "do using var x := OC.Text(new Res()); end; ",
            "i + 1")),
        "0 100\n");
}
