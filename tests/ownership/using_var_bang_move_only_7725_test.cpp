// The null-forgiving read `o!` of a MOVE-ONLY owning-choice local at a local
// binding MOVES the local, exactly as the bare read `o` does (issues #7725,
// #7726; spec §17.8 / §28.10 / §15.3, v0.350).
//
// THE GOVERNING TEXT.
//
//   §17.8 (v0.350) — "The null-forgiving operator preserves ownership: if `x`
//   has ownership state *owned* ..., `x!` is owned ... it does not move, copy,
//   or borrow beyond what reading `x` already does. Over a named owned
//   binding, `x!` is therefore classified by its position exactly as the bare
//   read `x` is ... At a **marked** position ... the transfer is spelled
//   `move x!`, the `!` being transparent to the marker".
//
//   §28.10 (v0.350) — "Where `x` is a local of **move-only value type**
//   (§15.3) the unmarked form **adopts**: ... the plain read moves ..., `y`
//   carries the value's owned slots to the end of *its* block and `x` is left
//   moved-from." The same section lists the null-forgiving read `x!` among the
//   spellings of the unmarked read ("classified as the bare `x` is").
//
//   §15.3 (v0.350) — the owning-classified choice and "its nullable wrapper
//   `C?`" are move-only; "Local-to-local assignment `var y := x;` ... moves
//   for a move-only value type. After the move, `x` is in moved-from state
//   and may not be read until reassigned."
//
// THE BUG. Both halves of the move peeled no `!`: the ownership pass's
// `noteOwningChoiceWholeMoveRead` and codegen's local-target
// `markOwningChoiceLocalMovedFrom` accepted only a bare identifier (or
// `move` over one). So `using var x := o!;` (#7725) and
// `using var x := (move w)!;` (#7726) left the source slot holding the payload
// the binding also held: both scope-end active-case passes disposed it
// (heap-use-after-free under ASan), and a later read of `o` was accepted.
//
// THE ORACLE. Each accept row runs the shape 100 times with a COUNTING
// destructor and pins the exact count (100), so a doubled disposal (crash or
// >100) and a suppressed one (<100, a leak) are both distinguishable from the
// spec's answer. Each reject row pins the SPECIFIC moved-from diagnostic; the
// pre-fix compiler accepted those programs. Each reject row's accepting twin
// is the accept row with the same binding spelling and no later read.
//
// per §17.8, §28.10, §15.3, v0.350

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// `body` runs once per iteration inside `T`, after `o` is bound to a live
// owning-choice value carrying one `Res`.
std::string prog(const std::string& body) {
    return std::string(
               "module M; "
               "public class Res "
               "  public static Count: int := 0; "
               "  public constructor() end constructor; "
               "  destructor Res.Count := Res.Count + 1; end destructor; "
               "end Res; "
               "public choice OC Text(owned r: Res), Empty; end OC; "
               "public class Program "
               "  public static method T() "
               "    var o: OC? := OC.Text(new Res()); ") +
           body +
           "  end T; "
           "  public static method Main(): int "
           "    for var i in 0..<100 do T(); end; "
           "    Console.WriteLine($\"{Res.Count}\"); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

void expectExactlyOneDisposalPerIteration(const JitResult& r) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

void expectMovedFromRead(const JitResult& r) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "was already moved out"));
}

}  // namespace

// --- #7725: `using var x := o!;` adopts and leaves `o` moved-from -----------

BEZEL_TEST(ownership_using_var_bang_move_only_7725,
           spec_28_10_using_var_bang_over_nullable_owning_choice_local_moves_it) {
    // The issue's repro shape. Pre-fix: heap-use-after-free (both `o` and `x`
    // disposed the one `Res`).
    expectExactlyOneDisposalPerIteration(
        runAot(prog("using var x := o!; ")));
}

BEZEL_TEST(ownership_using_var_bang_move_only_7725,
           spec_17_8_read_of_source_after_using_var_bang_is_rejected) {
    // Reject row; its accepting twin is the row above. §15.3: "may not be read
    // until reassigned". Pre-fix: accepted (and a heap-use-after-free).
    expectMovedFromRead(
        runAot(prog("using var x := o!; var n := o; ")));
}

BEZEL_TEST(ownership_using_var_bang_move_only_7725,
           spec_15_3_var_bang_over_nullable_owning_choice_local_moves_it) {
    // §17.8 is position-classified, not `using`-specific: the plain `var`
    // binding is §15.3's own local-to-local move spelling. Pre-fix: the same
    // heap-use-after-free.
    expectExactlyOneDisposalPerIteration(
        runAot(prog("var x := o!; _ := x; ")));
}

BEZEL_TEST(ownership_using_var_bang_move_only_7725,
           spec_17_8_read_of_source_after_var_bang_is_rejected) {
    // Reject row; accepting twin is the `var x := o!;` row above.
    expectMovedFromRead(
        runAot(prog("var x := o!; _ := x; var n := o; ")));
}

BEZEL_TEST(ownership_using_var_bang_move_only_7725,
           spec_15_3_assignment_bang_to_plain_local_moves_it) {
    // The `:=` spelling of the same local-to-local move. Pre-fix: the same
    // heap-use-after-free.
    expectExactlyOneDisposalPerIteration(
        runAot(prog("var x: OC := OC.Empty; x := o!; _ := x; ")));
}

BEZEL_TEST(ownership_using_var_bang_move_only_7725,
           spec_17_8_read_of_source_after_assignment_bang_is_rejected) {
    // Reject row; accepting twin is the assignment row above.
    expectMovedFromRead(runAot(
        prog("var x: OC := OC.Empty; x := o!; _ := x; var n := o; ")));
}

BEZEL_TEST(ownership_using_var_bang_move_only_7725,
           spec_28_10_using_var_plain_read_control_moves_it) {
    // Differential control (passes before and after the fix): the bare read
    // the `!` spelling must agree with.
    expectExactlyOneDisposalPerIteration(runAot(prog("using var x := o; ")));
}

// --- #7726: `(move w)!` / `move w!` are the same transfer -------------------

BEZEL_TEST(ownership_using_var_bang_move_only_7725,
           spec_17_8_using_var_paren_move_bang_moves_source_7726) {
    // The #7726 repro shape: the `!` applied to the marked transfer. Pre-fix:
    // heap-use-after-free (the `move` was recorded by the ownership pass, but
    // codegen's mark declined the `!`-wrapped spelling and never zeroed `o`).
    expectExactlyOneDisposalPerIteration(
        runAot(prog("using var x := (move o)!; ")));
}

BEZEL_TEST(ownership_using_var_bang_move_only_7725,
           spec_17_8_using_var_move_bang_moves_source_7726) {
    // §17.8's own marked spelling `move x!` ("the `!` being transparent to the
    // marker"). Pre-fix: the same heap-use-after-free.
    expectExactlyOneDisposalPerIteration(
        runAot(prog("using var x := move o!; ")));
}

BEZEL_TEST(ownership_using_var_bang_move_only_7725,
           spec_17_8_read_of_source_after_paren_move_bang_is_rejected_7726) {
    // Already rejected before the fix (the ownership pass records the nested
    // `move`); pinned so the compile-time half of #7726 stays.
    expectMovedFromRead(
        runAot(prog("using var x := (move o)!; var n := o; ")));
}
