// The `using var` COPY of an OWNED PARAMETER of a MOVE-ONLY value type must
// MOVE the value — leaving the SOURCE moved-from (issue #7415; spec §28.10 /
// §15.3 / §28.7 / §35.4, v0.345).
//
// THE GOVERNING TEXT.
//
//   docs/Bezel_Language_Spec.md:7270 (§28.10, v0.345) — "Where `x` is a local
//   of **move-only value type** (§15.3) the unmarked form **adopts**: a
//   move-only binding has no borrow reading to disambiguate, so the plain read
//   moves (K3.12; §15.3's *Local-to-local assignment*, and the announcement
//   table's row 8 for the call form), `y` carries the value's owned slots to
//   the end of *its* block and `x` is left moved-from."
//
//   docs/Bezel_Language_Spec.md:2839 (§15.3, v0.345) — "**Local-to-local
//   assignment** `var y := x;`: copies for an ordinary value type; moves for a
//   move-only value type. After the move, `x` is in moved-from state and may
//   not be read until reassigned."
//
//   docs/Bezel_Language_Spec.md:2841 (§15.3, v0.345) — "**Parameter passing**:
//   … for a move-only V, it is by value-move (transfer ownership). … A
//   parameter declared `(owned x: V)` is identical to plain `(x: V)` for a
//   move-only V; the modifier is admitted for documentation but is redundant."
//
//   docs/Bezel_Language_Spec.md:2844 (§15.3, v0.345) — "**Local lifetime**: …
//   A **whole-value** move — a plain binding, a plain argument, a return —
//   creates no such obligation on a **local**: it leaves the local holding no
//   value rather than a hole, so no pass walks it (K1's moved-from state:
//   'discharge machinery skips it'; §28.3's auto-drop runs only on a local 'not
//   in moved-from state'), and a read before reassignment is already an
//   error."
//
//   docs/Bezel_Language_Spec.md:2833 (§15.3, v0.345) — "A move-only value type
//   is a value type whose instances cannot be duplicated. Assignment transfers
//   the value from source to destination, leaving the source in moved-from
//   state. Reading from a moved-from location is a compile error."
//
// THE BUG (#7415). The adoption's MOVE half was implemented for a LOCAL source
// only (§15.3's *Local-to-local assignment*, and its codegen twin #4566's
// runtime zero-store over the source slot). A PARAMETER source was declined on
// both halves: the ownership pass's `noteOwningChoiceWholeMoveRead` returned
// on a parameter symbol, and codegen's `markOwningChoiceMovedFromCore`
// returned on a source that is not an `ast::LocalDecl`-backed local ("a
// parameter … neither is enrolled in the scope-end pass, so neither has a
// disposal to suppress" — a premise that is FALSE for an `owned`
// owning-choice parameter, which `registerUnwindOwnedParam`'s owning-choice
// arm DOES enroll in the scope-end active-case pass).
//
// So the copy of an owned owning-choice parameter did not move the value: the
// copy's scope-end active-case pass disposed the payload AND the parameter's
// own scope-end pass disposed it again. MEASURED (the issue's own repro, under
// `--sanitize=address`):
//
//   DTOR
//   ==1824386==ERROR: AddressSanitizer: heap-use-after-free on address 0x…
//   READ of size 8 at 0x… thread T0
//       #0 … in bz$Gowning_choice_copy_param_source.Program$Take
//   freed by thread T0 here:
//       #1 … in bz$Gowning_choice_copy_param_source.Program$Take
//
// — the free AND the read are BOTH inside the callee: the copy freed the
// block and the parameter's own disposal then read it. The caller is
// unaffected, because it moved the value away (§15.3) and never touches it
// again.
//
// THE ORACLE. Every accept row pins the DISPOSAL COUNT, not "did not crash":
// the destructor prints, so a doubled disposal and a suppressed one are
// distinguishable from each other and from the spec's answer, and the
// loop-amplified row pins the count over 500 iterations so a one-shot
// free-of-freed-memory is not the whole story. `ran`, `exitCode` and
// `stdoutText` are asserted together, because `runAot` reports a signal death
// as `ran == false` / `exitCode == -1`.
//
// The reject row pins the SPECIFIC diagnostic ("was already moved out"), not
// a bare `!r.ran` — the pre-fix compiler ACCEPTED the program, so the reject
// text is the only thing that distinguishes the two states.
//
// per §28.10, §15.3, §28.7, §35.4, v0.345

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// A destructor that PRINTS: one line per disposal, so a doubled disposal
// shows up as a duplicated line rather than as an indistinguishable crash.
const char* kNamedDecls =
    "class Res "
    "  public constructor() end constructor; "
    "  destructor Console.WriteLine(\"DTOR\"); end destructor; "
    "end Res; "
    "choice OChoice Text(owned r: Res), Empty; end OChoice; ";

// A COUNTING destructor for the loop-amplified row: the exact count over many
// iterations is the strong oracle — a count ABOVE the number of constructions
// is the double disposal, a count BELOW it is a suppression grown into a leak.
const char* kCountingDecls =
    "class Res "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res._n := Res._n + 1; end destructor; "
    "end Res; "
    "choice OChoice Text(owned r: Res), Empty; end OChoice; ";

// The CALLEE under test: the copy's spelling is the parameter of `take`.
std::string callee(const std::string& take) {
    return "public static method Take(" + take + ") " +
           "using var x := src; " + "end Take; ";
}

// The CALLER: the payload is built, handed to the callee, and the callee's
// disposal is the only one — the caller moved the value away (§15.3), so the
// caller's own binding holds nothing.
std::string caller() {
    return "public static method Main() "
           "var osrc := new Res(); "
           "var src := OChoice.Text(move osrc); "
           "Take(move src); "
           "Console.WriteLine(\"END\"); "
           "end Main; ";
}

std::string prog(const char* decls, const std::string& members) {
    return std::string("module M; ") + decls + " class Program " + members +
           " end Program;";
}

// Assert the program RAN to a clean exit and produced exactly `expected` on
// stdout. Split out because every row needs all three assertions.
void expectRunOutput(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// --- §28.10: the unmarked copy of an `owned` move-only parameter ADMITS ----

BEZEL_TEST(ownership_using_var_copy_owned_param_7415,
           spec_28_10_using_var_copy_of_an_owned_owning_choice_param_moves_it) {
    // The issue's own repro, verbatim: the callee copies its `owned`
    // owning-choice parameter into a `using var` binding. §28.10's move-only
    // adoption paragraph makes that copy a MOVE: the binding carries the
    // value's owned slots to the end of ITS block and the PARAMETER is left
    // moved-from. So exactly one disposal happens, in the callee.
    //
    // Pre-fix: the copy's scope-end pass disposed the payload and the
    // parameter's own scope-end pass then read the freed block —
    // `AddressSanitizer: heap-use-after-free … READ of size 8`, the free and
    // the read both in `Program$Take`.
    expectRunOutput(
        runAot(prog(kNamedDecls,
                    callee("owned src: OChoice") + caller())),
        std::string("DTOR\nEND\n"));
}

// --- §28.1 clause 1 + §15.3: the plain parameter is the SAME binding -------

BEZEL_TEST(ownership_using_var_copy_owned_param_7415,
           spec_15_3_plain_move_only_parameter_is_identical_to_the_owned_one) {
    // §15.3's *Parameter passing*: "A parameter declared `(owned x: V)` is
    // identical to plain `(x: V)` for a move-only V; the modifier is admitted
    // for documentation but is redundant." The plain spelling is the one §15.3
    // names at the CALL SITE ("at either parameter spelling the argument is
    // written **unmarked**"), so the copy of a plain move-only parameter is
    // the same MOVE — and the value is disposed exactly once, by the copy.
    //
    // This row is the differential control for the row above: the two
    // spellings must agree on the disposal count.
    expectRunOutput(
        runAot(prog(kNamedDecls,
                    callee("src: OChoice") +
                    "public static method Main() "
                    "var osrc := new Res(); "
                    "var src := OChoice.Text(move osrc); "
                    "Take(src); "
                    "Console.WriteLine(\"END\"); "
                    "end Main; ")),
        std::string("DTOR\nEND\n"));
}

// --- §15.3 *Local-to-local assignment*: the plain binding moves too --------

BEZEL_TEST(ownership_using_var_copy_owned_param_7415,
           spec_15_3_var_binding_of_an_owned_owning_choice_param_moves_it) {
    // §15.3's own spelling of the move: "`var y := x;`: … moves for a move-only
    // value type." The `var` form is the same transfer as the `using var`
    // form — only the binding's scope-end obligation differs — so it must
    // leave the parameter moved-from as well, and the payload is disposed
    // exactly once.
    //
    // Pre-fix: the same heap-use-after-free as the `using var` spelling (the
    // parameter's own scope-end pass read the block the copy had freed).
    expectRunOutput(
        runAot(prog(kNamedDecls,
                    "public static method Take(owned src: OChoice) "
                    "var x := src; "
                    "end Take; " + caller())),
        std::string("DTOR\nEND\n"));
}

// --- The `move` spelling is the SAME transfer --------------------------------

BEZEL_TEST(ownership_using_var_copy_owned_param_7415,
           spec_15_3_move_spelled_copy_of_an_owned_owning_choice_param_moves_it) {
    // §15.3's *Local-to-local assignment* scope note: the plain binding stays
    // UNMARKED, "as this rule has always defined" — but the `move` spelling is
    // the same transfer, and it must leave the parameter moved-from too.
    expectRunOutput(
        runAot(prog(kNamedDecls,
                    "public static method Take(owned src: OChoice) "
                    "using var x := move src; "
                    "end Take; " + caller())),
        std::string("DTOR\nEND\n"));
}

// --- §15.3: a moved-from location may not be READ --------------------------

BEZEL_TEST(ownership_using_var_copy_owned_param_7415,
           spec_15_3_a_moved_from_owning_choice_parameter_may_not_be_read) {
    // §15.3: "Reading from a moved-from location is a compile error." The
    // adoption left the parameter moved-from, so the read after it is the
    // error — not a runtime fault.
    //
    // Pre-fix this was a CLEAN COMPILE, and the read yielded the freed
    // payload pointer (a heap-use-after-free under ASan), so the reject text
    // is the only thing that separates the two states.
    const JitResult r = runAot(prog(kNamedDecls,
                                    "public static method Take(owned src: OChoice) "
                                    "using var x := src; "
                                    "Console.WriteLine(src); "
                                    "end Take; " + caller()));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "was already moved out"));
}

// --- §28.1 clause 1: the plain parameter is left moved-from too -------------

BEZEL_TEST(ownership_using_var_copy_owned_param_7415,
           spec_15_3_a_moved_from_plain_move_only_parameter_may_not_be_read) {
    // The same rule at the PLAIN spelling, which §15.3's *Parameter passing*
    // makes identical to the `owned` one for a move-only V ("the modifier is
    // admitted for documentation but is redundant"). So the copy of a plain
    // move-only parameter moves it too, and the parameter is left moved-from.
    //
    // The SIBLING move-only kind already behaves this way: the inline-array
    // twin's parameter arm (`noteMoveOnlyArrayWholeMoveRead`, #1922) seeds the
    // parameter into the flow state at the move site and leaves it moved-from,
    // so a read after the copy is rejected with this same diagnostic. This row
    // is that row's owning-choice twin.
    const JitResult r = runAot(prog(kNamedDecls,
                                    "public static method Take(src: OChoice) "
                                    "using var x := src; "
                                    "Console.WriteLine(src); "
                                    "end Take; "
                                    "public static method Main() "
                                    "var osrc := new Res(); "
                                    "var src := OChoice.Text(move osrc); "
                                    "Take(src); "
                                    "Console.WriteLine(\"END\"); "
                                    "end Main; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "was already moved out"));
}

// --- The loop-amplified count -----------------------------------------------

BEZEL_TEST(ownership_using_var_copy_owned_param_7415,
           spec_15_3_every_copied_payload_in_a_loop_is_disposed_exactly_once) {
    // 500 iterations, one `Res` each, each copied out of the callee's
    // parameter into a `using var` binding. §28.10's adoption paragraph puts
    // the payload in the copy, so the count is exactly 500 — one per
    // construction. A count ABOVE 500 is the double disposal this issue is
    // about; a count BELOW it is a suppression that has grown into a leak.
    expectRunOutput(
        runAot(prog(kCountingDecls,
                    "public static method Take(owned src: OChoice) "
                    "using var x := src; "
                    "end Take; "
                    "public static method Churn() "
                    "var i := 0; "
                    "while i < 500 do "
                    "var osrc := new Res(); "
                    "var src := OChoice.Text(move osrc); "
                    "Take(move src); "
                    "i := i + 1; "
                    "end while; "
                    "end Churn; "
                    "public static method Main() "
                    "Churn(); "
                    "Console.WriteLine(Res._n); "
                    "end Main; ")),
        std::string("500\n"));
}

// --- The differential control: the LOCAL source already moved ---------------

BEZEL_TEST(ownership_using_var_copy_owned_param_7415,
           spec_15_3_local_source_copy_disposes_the_payload_exactly_once) {
    // The pre-existing half of the family (§15.3's *Local-to-local
    // assignment*, codegen twin #4566): the copy of a LOCAL owning-choice
    // binding moves it, so the payload is disposed exactly once. The rows
    // above are the SAME shape with the source moved from a local to a
    // parameter; this row is the control that says what the parameter rows
    // must now look like.
    expectRunOutput(
        runAot(prog(kNamedDecls,
                    "public static method Take() "
                    "var osrc := new Res(); "
                    "var src := OChoice.Text(move osrc); "
                    "using var x := src; "
                    "end Take; "
                    "public static method Main() "
                    "Take(); "
                    "Console.WriteLine(\"END\"); "
                    "end Main; ")),
        std::string("DTOR\nEND\n"));
}
