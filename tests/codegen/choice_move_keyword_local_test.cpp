// Codegen regression test for the §15.3 bullet 1 WHOLE-VALUE MOVE written with
// the `move` KEYWORD: `var t := move src;`, spec v0.334.
//
// NO ISSUE NUMBER: this defect has no filed issue of its own, and borrowing
// #4566's would misattribute it. It is the SIBLING of two real ones on the same
// machinery — #4566 (the bare `var c := b;` and `c := b;` spellings) and #5436
// (the by-value-move call ARGUMENT) — and the third spelling of the same
// transfer that neither of them covered.
//
// §15.3 *Move-only value types*, first *Rules* bullet, scopes the unmarked
// binding: "A move-only value binding has no borrow reading to disambiguate —
// copies are forbidden, so the plain binding can only move — and therefore
// stays unmarked, as this rule has always defined." That sentence is about
// which spelling the RULE requires; it is not a licence for the marked spelling
// to be a different transfer. A.7 admits `MoveExpr ::= "move" PrimaryExpr`, the
// compiler ACCEPTS `var t := move src;` and lowers it as the same whole-value
// move, so the source must be left in moved-from state on that path too.
//
// §15.3 *Local lifetime*: a whole-value move "leaves the local holding no value
// rather than a hole, so no pass walks it (K1's moved-from state: 'discharge
// machinery skips it'; §28.3's auto-drop runs only on a local 'not in moved-from
// state')". §28.7 *Moved-from and null* says the same for the next store: "A
// moved-from or null location therefore stores without disposal."
//
// THE BUG. `markOwningChoiceMovedFromCore` (LlvmBackend_Disposal.cpp) read its
// source with a bare `dynamic_cast<ast::IdentifierExpr*>`, so the `move` node —
// an `ast::UnaryExpr` with `op == "move"` wrapping the identifier — was declined
// and the source kept its scope-end owning-choice disposal. BOTH slots then
// held the same owned payload pointer and BOTH active-case passes dispatched
// `Dispose` through it: a double free. The sibling `nullTransferredOwnedLocalSlot`
// (LlvmBackend_Assign.cpp) has unwrapped exactly this node for the call-argument
// position all along, which is why `C.Sink(move b!)` never double-freed while
// this declaration form did.
//
// MEASURED at `r23-c28` (fece431f): the two pins below FAIL on that base and
// pass with the fix. Their pre-fix observable is a TIMEOUT under `runAot` —
// the double free corrupts the heap and the process hangs or dies — and
// `heap-use-after-free ... READ of size 8` in `main` under
// `--sanitize=address`, of a region allocated by the `new Resource(...)` in the
// initializer. The DIFFERENTIAL CONTROL is the second row: `var t := src;` —
// the identical transfer, unmarked — is clean on the SAME base build, so the
// marker alone decided the double free.
//
// THE ORACLE. A destructor that PRINTS pins the exact DISPOSAL COUNT and its
// order, so a doubled disposal and a suppressed one are distinguishable from
// each other and from the spec's answer. `ran` and `exitCode` are asserted too:
// a pre-fix double free is a signal death or a hang, which `runAot` reports as
// `ran == false` or as the TIMEOUT the harness prints. The loop row counts over
// many iterations because a ONE-SHOT
// double free can pass on both sides of a fix (the second dispatch may land in
// still-mapped memory).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// A destructor that PRINTS: one line per disposal, so a doubled disposal is a
// duplicated line rather than an indistinguishable crash.
const char* kNamedResourceDecls =
    "class Resource "
    "  private _name: string; "
    "  public constructor(name: string) _name := name; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_name}\"); end destructor; "
    "end Resource; "
    "choice Bundle Ready(owned r: Resource), Empty; end Bundle; ";

// A COUNTING destructor for the loop-amplified row: only an exact count over
// many iterations distinguishes "disposed twice" from "disposed once".
const char* kCountingResourceDecls =
    "class Resource "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Resource._n := Resource._n + 1; end destructor; "
    "end Resource; "
    "choice Bundle Ready(owned r: Resource), Empty; end Bundle; ";

bezel::test::JitResult run(const std::string& decls, const std::string& body,
                           const std::string& extraMembers = "") {
    return runAot(std::string("module M; ") + decls +
                  " class Host public static method Main() " + body +
                  " end Main; " + extraMembers + " end Host;");
}

// Assert the program RAN to a clean exit and produced exactly `expected` on
// stdout. All three assertions are needed: a pre-fix double free is a signal
// death, reported as ran == false with a truncated stdout.
void expectRunOutput(const bezel::test::JitResult& r,
                     const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// --- §15.3 bullet 1, `move`-SPELLED binding: the bug itself ------------------

BEZEL_TEST(codegen_choice_move_keyword_local,
           spec_15_3_var_binding_move_keyword_moves_the_owning_choice) {
    // The declaration form with the `move` keyword. Pre-fix: `moved` then
    // SIGSEGV (and heap-use-after-free, READ of size 8, under ASan) because
    // `src`'s scope-end pass still dispatched `Dispose` through the payload
    // `t` had taken. Post-fix: `t` is the sole owner and the payload is
    // disposed exactly once.
    expectRunOutput(
        run(kNamedResourceDecls,
            "var src := Bundle.Ready(new Resource(\"src\")); "
            "var t := move src; "
            "Console.WriteLine(\"moved\");"),
        std::string("moved\ndispose src\n"));
}

// --- the DIFFERENTIAL CONTROL: the same transfer, unmarked -------------------

BEZEL_TEST(codegen_choice_move_keyword_local,
           spec_15_3_var_binding_unmarked_is_the_same_transfer_control) {
    // The control the fix is judged against. §15.3 bullet 1's scope note
    // makes the PLAIN binding the unmarked spelling, and that path already
    // vacated the source. This row is green on the base build and must STAY
    // green: it is what shows the `move` marker — not the transfer itself —
    // was what lost the vacated state.
    expectRunOutput(
        run(kNamedResourceDecls,
            "var src := Bundle.Ready(new Resource(\"src\")); "
            "var t := src; "
            "Console.WriteLine(\"moved\");"),
        std::string("moved\ndispose src\n"));
}

// --- §28.7: the next store into the moved-from source drops nothing ---------

BEZEL_TEST(codegen_choice_move_keyword_local,
           spec_28_7_store_into_a_move_keyword_moved_from_source_drops_nothing) {
    // "A moved-from or null location therefore stores without disposal." After
    // `var t := move src;` the payload belongs to `t`, so the fresh store into
    // `src` must NOT run §35.4's drop-on-overwrite on the stale bytes. Two
    // resources are constructed; each is disposed exactly once, and the ORDER is
    // reverse declaration order — `t` was declared after `src`, so `t` (holding
    // the original `src` payload) is disposed before `src` (holding `src2`).
    //
    // NOT a fail-without pin, and deliberately labelled so: MEASURED at the base
    // this row is GREEN both before and after the fix. The pre-fix observable
    // for this SHAPE is not an extra printed disposal — the base binary hangs or
    // dies in the other two rows, and this one's stdout still comes out right.
    // What the row guards is the OTHER direction: that the fix does not suppress
    // a disposal it owes, which would convert a double free into a LEAK. A
    // suppressed `t` would print `dispose src` once here instead of the two
    // resources' worth this asserts.
    expectRunOutput(
        run(kNamedResourceDecls,
            "var src := Bundle.Ready(new Resource(\"src\")); "
            "var t := move src; "
            "src := Bundle.Ready(new Resource(\"src2\")); "
            "Console.WriteLine(\"reacquired\");"),
        std::string("reacquired\ndispose src\ndispose src2\n"));
}

// --- loop-amplified: the exact disposal COUNT over many iterations ----------

BEZEL_TEST(codegen_choice_move_keyword_local,
           spec_15_3_move_keyword_transfer_disposes_exactly_once_loop) {
    // 200 iterations of the same transfer, each in its OWN block so `t`'s
    // scope-end disposal runs inside the iteration. The pre-fix double free is
    // not reliably fatal one-shot (the second dispatch may land in still-mapped
    // memory — MEASURED: 59 signals in 300 one-shot runs of a sibling shape,
    // and 0 in the other 241), so the strong oracle is the COUNT: each
    // iteration constructs one Resource and must dispose it exactly once — 200
    // disposals, never 400. A count above 200 is the double free; a count below
    // is a suppression that has become a leak.
    bezel::test::JitResult r =
        run(kCountingResourceDecls,
            "Host.Churn(); Console.WriteLine(Resource._n);",
            "public static method Churn() "
            "  var i := 0; "
            "  while i < 200 do "
            "    var src := Bundle.Ready(new Resource()); "
            "    var t := move src; "
            "    i := i + 1; "
            "  end while; "
            "end Churn;");
    expectRunOutput(r, std::string("200\n"));
}