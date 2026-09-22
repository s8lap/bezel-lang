// Codegen regression tests for issue #4617: §35.5 *The owning match* —
// arm-selection payload transfer is PER SLOT, not per arm (spec v0.307).
//
// THE BUG. §35.5 states both halves of arm selection at slot granularity: the
// selected arm "transfers the case's `owned` and move-only value payloads into
// its bindings", and "the un-transferred remainder — unlisted fields, unmarked
// named slots, discarded siblings — is disposed at construct completion".
// #4609 implemented the second half at ARM granularity: `patternBindsAnySlot`
// gated a single zero-store over the whole captured scrutinee, so the reclaim
// was discharged for EVERY slot as soon as the arm bound ANY slot. An arm that
// binds some slots of a multi-payload case therefore left its un-bound
// siblings reclaimed by nobody. #4609's own comment said so: "a binding arm's
// UN-transferred siblings (`Fetched(var w, _)`) still leak rather than
// double-free — the sound direction".
//
// MEASURED on this lane's HEAD before the fix, over
// `W(owned a: Widget, owned b: Widget)`:
//
//     case move o of
//       W(var w, _) => do using var live := move w; end,
//       X           => ()
//     end case;
//
//     adopted / dispose a / after          ← `dispose b` never printed
//
// and, loop-amplified to 200 constructs with a counting destructor, 200
// disposals where §35.5 requires 400. Post-fix: `adopted / dispose a /
// dispose b / after`, and exactly 400.
//
// THE ORACLE is the exact DISPOSAL COUNT and the exact ORDER, never "it did
// not crash": a printing destructor gives one line per disposal, so a
// suppressed disposal, a doubled one and a mis-ordered one are all
// distinguishable. The amplified row uses a counting destructor over 200
// iterations rather than a one-shot LSan probe, because a single leaked
// allocation can stay reachable from a live stack slot and pass on both sides
// of the fix. `r.ran` is asserted on every row because a double free — the
// error direction this fix must NOT introduce — is a signal death that
// `runAot` reports as `ran == false` with truncated stdout.
//
// THE CONTROLS matter as much as the repro. The fix narrows what is marked
// moved-from, and narrowing it too far is a DOUBLE FREE, not a leak. The
// all-slots-bound row and the two edge rows (`return` out of the arm, `throw`
// out of the arm) pin that every slot the arm really adopts is still marked.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Two `owned` payloads on one case — the shape the per-arm discharge got
// wrong. A printing destructor names which payload was disposed, so a row
// pins WHICH sibling leaked rather than just how many did.
const char* kTwoPayloadDecls =
    "public class Widget "
    "  private _n: string; "
    "  public constructor(n: string) _n := n; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_n}\"); end destructor; "
    "  public method Ping(): int => 7; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; ";

// The same choice reached through a fresh producer — §35.5's FIRST announcing
// scrutinee form, "an unbound owned temporary", which shares the discharge.
const char* kProducerDecls =
    "public class Widget "
    "  private _n: string; "
    "  public constructor(n: string) _n := n; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_n}\"); end destructor; "
    "  public method Ping(): int => 7; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; "
    "public class Client "
    "  public constructor() end constructor; "
    "  public method Fetch(): Outer => "
    "    Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
    "end Client; ";

// A COUNTING destructor for the amplified row: the exact count over many
// iterations is the only oracle that separates "disposed once each" from both
// "one sibling leaked" and "some payload disposed twice".
const char* kCountingTwoPayloadDecls =
    "public class Widget "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Widget._n := Widget._n + 1; end destructor; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; ";

// An owned / value / owned payload triple: the middle slot binds but owns
// nothing, so it exercises the per-slot mark on a non-owning field.
const char* kMixedPayloadDecls =
    "public class Widget "
    "  private _n: string; "
    "  public constructor(n: string) _n := n; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_n}\"); end destructor; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, n: int, owned b: Widget), X; "
    "end Outer; ";

bezel::test::JitResult run(const std::string& decls, const std::string& body) {
    return runAot(std::string("module M; ") + decls +
                  " public class Program public static method Main(): int " +
                  body + " end Main; end Program;");
}

// The same program under generated-code ASan/LSan. `runAotChecked` flips
// `ran` to false on a leak, a double free or a use-after-free, so it is the
// direct oracle for BOTH error directions of this fix — and the only one that
// sees a leaked block the program never prints about.
bezel::test::JitResult runChecked(const std::string& decls,
                                  const std::string& body) {
    return runAotChecked(std::string("module M; ") + decls +
                         " public class Program public static method Main(): int " +
                         body + " end Main; end Program;");
}

// Every row needs all three: the error direction this fix must not introduce
// is a double free, which `runAot` reports as ran == false with truncated
// stdout rather than as a wrong string.
void expectRun(const bezel::test::JitResult& r, const std::string& out,
               int exitCode) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// --- THE BUG: the issue's repro --------------------------------------------

BEZEL_TEST(codegen_owning_match_per_slot_transfer_4617,
           spec_35_5_unbound_sibling_of_a_partially_binding_arm_is_disposed) {
    // The issue's own program. `W(var w, _)` transfers slot `a` into `w`
    // (adopted by the `using var`, disposed at the `do … end` close) and
    // transfers NOTHING for slot `b`, which §35.5 leaves in "the
    // un-transferred remainder … disposed at construct completion" — i.e.
    // after the arm body and before "after". Pre-fix output was
    // `adopted / dispose a / after`: `b` was reclaimed by nobody.
    expectRun(
        run(kTwoPayloadDecls,
            "var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
            "case move o of "
            "  W(var w, _) => do "
            "    using var live := move w; "
            "    Console.WriteLine(\"adopted\"); "
            "  end, "
            "  X => () "
            "end case; "
            "Console.WriteLine(\"after\"); "
            "return 0;"),
        std::string("adopted\ndispose a\ndispose b\nafter\n"), 0);
}

BEZEL_TEST(codegen_owning_match_per_slot_transfer_4617,
           spec_35_5_partial_binding_over_200_constructs_disposes_exactly_2n) {
    // The strong oracle. A single leaked allocation can remain reachable from
    // a live stack slot and read clean under LSan on BOTH sides of the fix, so
    // amplify and count: 200 constructs, two Widgets each, one slot adopted
    // and one discarded. 400 is §35.5's answer; 200 is the pre-fix leak (one
    // sibling per construct), and anything above 400 is a double free.
    expectRun(
        run(kCountingTwoPayloadDecls,
            "var i := 0; "
            "while i < 200 do "
            "  var o := Outer.W(new Widget(), new Widget()); "
            "  case move o of "
            "    W(var w, _) => do using var live := move w; end, "
            "    X => () "
            "  end case; "
            "  i := i + 1; "
            "end while; "
            "Console.WriteLine(Widget._n); "
            "return 0;"),
        std::string("400\n"), 0);
}

BEZEL_TEST(codegen_owning_match_per_slot_transfer_4617,
           spec_35_5_partial_binding_under_an_unbound_temporary_scrutinee) {
    // §35.5's OTHER announcing scrutinee form — "an unbound owned temporary …
    // an owning-choice-returning call" — reaches the same discharge, so it
    // carried the same per-arm defect. Pre-fix: `adopted / dispose a`, with
    // `b` leaked.
    expectRun(
        run(kProducerDecls,
            "using var c := new Client(); "
            "case c.Fetch() of "
            "  W(var w, _) => do "
            "    using var live := move w; "
            "    Console.WriteLine(\"adopted\"); "
            "  end, "
            "  X => () "
            "end case; "
            "Console.WriteLine(\"after\"); "
            "return 0;"),
        std::string("adopted\ndispose a\ndispose b\nafter\n"), 0);
}

BEZEL_TEST(codegen_owning_match_per_slot_transfer_4617,
           spec_35_5_value_slot_between_two_owned_slots_routes_per_slot) {
    // A binding on a slot that owns nothing must not disturb its owned
    // siblings' routing: `k` binds a copy of the `int` payload (§35.5
    // "**Copyable** value payloads bind as copies"), `w` adopts `a`, and `b`
    // stays in the remainder. Pre-fix the whole value was marked and `b`
    // leaked; the row also pins that marking the non-owning slot is harmless.
    expectRun(
        run(kMixedPayloadDecls,
            "var o := Outer.W(new Widget(\"a\"), 3, new Widget(\"b\")); "
            "case move o of "
            "  W(var w, var k, _) => do "
            "    using var live := move w; "
            "    Console.WriteLine($\"k={k}\"); "
            "  end, "
            "  X => () "
            "end case; "
            "Console.WriteLine(\"after\"); "
            "return 0;"),
        std::string("k=3\ndispose a\ndispose b\nafter\n"), 0);
}

// --- THE EDGES: a `return` / `throw` out of a partially-binding arm ----------

BEZEL_TEST(codegen_owning_match_per_slot_transfer_4617,
           spec_35_5_return_out_of_a_partially_binding_arm_disposes_remainder) {
    // A `return` never reaches construct completion, so the remainder is
    // reclaimed by the dispose-stack entry over the captured scrutinee — which
    // now reads the PER-SLOT marks. Both must be disposed exactly once on this
    // edge: `a` by the arm's `using var`, `b` by the entry. Pre-fix `b` leaked
    // here too; a fix that marked too little would double-free `a`.
    expectRun(
        run(kTwoPayloadDecls,
            "var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
            "case move o of "
            "  W(var w, _) => do "
            "    using var live := move w; "
            "    Console.WriteLine(\"hit\"); "
            "    return 5; "
            "  end, "
            "  X => () "
            "end case; "
            "return 0;"),
        std::string("hit\ndispose a\ndispose b\n"), 5);
}

BEZEL_TEST(codegen_owning_match_per_slot_transfer_4617,
           spec_35_5_throw_out_of_a_partially_binding_arm_disposes_remainder) {
    // §35.5: "An exception out of an arm disposes the not-yet-consumed
    // remainder during unwinding (K3.15)" — the remainder, and the arm's owned
    // local, each exactly once. The unwind edge reads the same per-slot marks
    // as the normal edge, which is why the discharge is a runtime store into
    // the captured value rather than a compile-time skip.
    expectRun(
        run(kTwoPayloadDecls,
            "try "
            "  var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
            "  case move o of "
            "    W(var w, _) => do "
            "      using var live := move w; "
            "      throw new InvalidOperationException(\"boom\"); "
            "    end, "
            "    X => () "
            "  end case; "
            "catch "
            "  e: InvalidOperationException => Console.WriteLine(\"caught\") "
            "end try; "
            "Console.WriteLine(\"after\"); "
            "return 0;"),
        std::string("dispose a\ndispose b\ncaught\nafter\n"), 0);
}

// --- THE CONTROLS: shapes that were already right and must stay right -------

BEZEL_TEST(codegen_owning_match_per_slot_transfer_4617,
           control_every_slot_bound_and_adopted_disposes_each_exactly_once) {
    // The double-free guard. When the arm binds EVERY slot, the fix must mark
    // every slot — otherwise the completion reclaim disposes a block the arm's
    // `using var` already owns. Two disposals, in adoption-scope order, and a
    // clean exit (a double free is a signal death, i.e. `ran == false`).
    expectRun(
        run(kTwoPayloadDecls,
            "var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
            "case move o of "
            "  W(var w, var v) => do "
            "    using var l1 := move w; "
            "    using var l2 := move v; "
            "    Console.WriteLine(\"adopted\"); "
            "  end, "
            "  X => () "
            "end case; "
            "Console.WriteLine(\"after\"); "
            "return 0;"),
        std::string("adopted\ndispose b\ndispose a\nafter\n"), 0);
}

BEZEL_TEST(codegen_owning_match_per_slot_transfer_4617,
           control_all_slots_discarded_still_disposes_at_construct_completion) {
    // §35.5 *(b) Normal path, payload discarded*, with two payloads: the arm
    // binds nothing, transfers nothing, and BOTH payloads are disposed at
    // construct completion — before "after". Already correct before this fix
    // (the arm never reached the discharge at all); the row pins that the
    // per-slot rewrite did not turn the reclaim into a no-op.
    expectRun(
        run(kTwoPayloadDecls,
            "var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
            "case move o of "
            "  W(_, _) => Console.WriteLine(\"hit\"), "
            "  X       => () "
            "end case; "
            "Console.WriteLine(\"after\"); "
            "return 0;"),
        std::string("hit\ndispose a\ndispose b\nafter\n"), 0);
}

BEZEL_TEST(codegen_owning_match_per_slot_transfer_4617,
           control_unmarked_named_scrutinee_is_still_a_borrowing_match) {
    // §35.5 *The borrowing match*: "An unmarked read of a named owned binding
    // in scrutinee position stays a borrowing match" — the construct consumes
    // nothing, the bindings are borrows, and §35.4's scope-end active-case
    // pass remains the owner, so both disposals land AFTER "after". The fix
    // touches only the owning-match discharge; this row pins that the
    // borrowing spelling was left alone.
    expectRun(
        run(kTwoPayloadDecls,
            "var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
            "case o of "
            "  W(var w, _) => Console.WriteLine(w.Ping()), "
            "  X           => () "
            "end case; "
            "Console.WriteLine(\"after\"); "
            "return 0;"),
        std::string("7\nafter\ndispose a\ndispose b\n"), 0);
}


// --- THE SANITIZER ORACLE: LSan sees the leaked sibling directly ------------

BEZEL_TEST(codegen_owning_match_per_slot_transfer_4617,
           spec_35_5_partial_binding_loop_is_lsan_clean_under_asan) {
    // `runAotChecked` builds the program with ASan/LSan and reports a leak as
    // `ran == false`. LSan only reports UNREACHABLE allocations, so a one-shot
    // repro can keep its block reachable from a live stack slot and read clean
    // on BOTH sides of the fix — amplify to 500 constructs, which leaves 500
    // unreachable Widgets behind on the pre-fix compiler (one un-bound sibling
    // per iteration) and none after it. The printed count is the second half
    // of the oracle: 1000 disposals, i.e. every Widget of every construct.
    auto r = runChecked(
        kCountingTwoPayloadDecls,
        "var i := 0; "
        "while i < 500 do "
        "  var o := Outer.W(new Widget(), new Widget()); "
        "  case move o of "
        "    W(var w, _) => do using var live := move w; end, "
        "    X => () "
        "  end case; "
        "  i := i + 1; "
        "end while; "
        "Console.WriteLine(Widget._n); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1000\n"));
}

BEZEL_TEST(codegen_owning_match_per_slot_transfer_4617,
           control_every_slot_bound_loop_is_asan_clean_no_double_free) {
    // The other direction, under the same sanitizer. When the arm adopts EVERY
    // slot, marking too little would let the completion reclaim dispose a
    // block the arm's `using var` already freed — ASan reports that as
    // `heap-use-after-free` / `double-free`, i.e. `ran == false`. 500
    // iterations so a stale-pointer read cannot stay in still-mapped memory.
    auto r = runChecked(
        kCountingTwoPayloadDecls,
        "var i := 0; "
        "while i < 500 do "
        "  var o := Outer.W(new Widget(), new Widget()); "
        "  case move o of "
        "    W(var w, var v) => do "
        "      using var l1 := move w; "
        "      using var l2 := move v; "
        "    end, "
        "    X => () "
        "  end case; "
        "  i := i + 1; "
        "end while; "
        "Console.WriteLine(Widget._n); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1000\n"));
}
