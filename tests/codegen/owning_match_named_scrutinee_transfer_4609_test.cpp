// Codegen regression tests for issue #4609: §35.5 *The owning match* — the
// SECOND announcing scrutinee form, "a named owned binding under an explicit
// scrutinee transfer marker" (`case move outcome of …`) — spec v0.306.
//
// THE BUG. §35.5 says of that form: "the consumed binding is left
// **moved-from** when the construct completes". Codegen never recorded it.
// `unboundOwningChoiceTemp`'s own exclusion list stated the gap and misread
// its consequence — "the source local's slot is left populated and its
// scope-end pass still disposes it; #4552's follow-up" — but the source local
// is not the only owner after the construct: §35.5's owning match TRANSFERS
// the selected case's owned payloads into the arm's bindings, and its own
// adoption spelling (`using var u := move r;` in a `do … end` arm) disposes
// the payload at the arm's scope end. The source's scope-end active-case pass
// then dispatched `Dispose` through the SAME block a second time. Not a leak
// — a double free.
//
// MEASURED at the issue's repro (an owning choice nested in an owning choice)
// on this lane's HEAD before the fix:
//
//     $ bezelc --sanitize=address m2.bzl -o m2a.x && ./m2a.x
//     ==…==ERROR: AddressSanitizer: heap-use-after-free on address 0x…
//     READ of size 8 at 0x… thread T0
//
// and a plain SIGSEGV (exit 139) without the sanitizer. The defect is NOT
// specific to nesting: the single-level shape is rule-corpus case s35_5_0003,
// written from §35.5's own example, which printed its three expected lines and
// then SIGSEGV'd on the way out.
//
// THE DISCRIMINATOR, from the issue, is carried below as a CONTROL: the same
// program with an UNBOUND TEMPORARY scrutinee is clean on both sides of the
// fix (#4552 gave that form its construct-completion reclaim and gated it on
// the arms binding). Only the named binding corrupted, which is what makes
// "the marked scrutinee is never marked moved-from" the whole cause.
//
// THE ORACLE is the exact DISPOSAL COUNT, not "it did not crash". A printing
// destructor gives one line per disposal, so a doubled disposal and a
// suppressed one are distinguishable from each other and from the spec's
// answer; the loop-amplified row pins the count over 200 constructs, because a
// one-shot double free can land in still-mapped memory and pass on both sides.
// `r.ran` is asserted alongside stdout because `runAot` reports a signal death
// as `ran == false` with truncated output, which is the pre-fix observable.
//
// NOTE ON THE SECOND HALF OF THE FIX. The construct-completion reclaim's
// arms-bind gate became PER ARM (it was per construct). §35.5 disposes at
// completion only the payloads "not transferred into bindings", and a
// construct mixing a binding arm with a discarding one had the reclaim
// disabled on *every* arm — so selecting the discarding arm leaked. The mixed
// row below pins that; it is a guard on this fix's own precision rather than a
// FAIL-WITHOUT row (pre-fix the source's scope-end pass happened to cover it).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// A PRINTING destructor, so a row pins both the fact and the moment of every
// disposal and a doubled disposal shows as a duplicated line rather than as an
// indistinguishable crash. `Inner` nested inside `Outer` is the issue's own
// shape: §35.4 rule 1 makes `Inner` owning (an `owned` class payload) and
// therefore a move-only value type, which makes `Outer` owning in turn.
const char* kNestedDecls =
    "public class Widget "
    "  public constructor() end constructor; "
    "  destructor Console.WriteLine(\"widget disposed\"); end destructor; "
    "  public method Ping(): int => 7; "
    "end Widget; "
    "public choice Inner A(owned v: Widget), B; end Inner; "
    "public choice Outer W(owned inner: Inner), X; end Outer; ";

// The single-level shape — rule-corpus case s35_5_0003's declarations.
const char* kFlatDecls =
    "public class Widget "
    "  public constructor() end constructor; "
    "  destructor Console.WriteLine(\"widget disposed\"); end destructor; "
    "  public method Ping(): int => 7; "
    "end Widget; "
    "public choice Outer W(owned v: Widget), X; end Outer; ";

// A COUNTING destructor for the loop-amplified row: the exact count over many
// iterations is the only oracle that separates "disposed once" from both
// "disposed twice" and "never disposed".
const char* kCountingFlatDecls =
    "public class Widget "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Widget._n := Widget._n + 1; end destructor; "
    "  public method Ping(): int => 7; "
    "end Widget; "
    "public choice Outer W(owned v: Widget), X; end Outer; ";

// Two owned-payload cases, so a construct can bind in one arm and discard in
// the other — the shape the whole-construct arms-bind gate got wrong.
const char* kTwoCaseDecls =
    "public class Widget "
    "  public constructor() end constructor; "
    "  destructor Console.WriteLine(\"widget disposed\"); end destructor; "
    "  public method Ping(): int => 7; "
    "end Widget; "
    "public choice Outer A(owned a: Widget), B(owned b: Widget); end Outer; ";

bezel::test::JitResult run(const std::string& decls, const std::string& body) {
    return runAot(std::string("module M; ") + decls +
                  " public class Program public static method Main(): int " +
                  body + " end Main; end Program;");
}

// Every row needs all three: a pre-fix double free is a signal death, which
// `runAot` reports as ran == false with empty-or-truncated stdout.
void expectRun(const bezel::test::JitResult& r, const std::string& out,
               int exitCode) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// --- THE BUG: the issue's repro, an owning choice inside an owning choice ---

BEZEL_TEST(codegen_owning_match_named_scrutinee_transfer_4609,
           spec_35_5_nested_owning_choice_payload_disposed_exactly_once) {
    // The issue's repro. §35.5: through "a move-only choice payload … nested
    // destructuring **routes recursively** in an owning match, and the
    // recursion runs exactly through the slots that **transfer**". `W(var i)`
    // is a positional slot, so `Inner` transfers into `i`; the inner
    // `case move i of` then transfers the Widget into `w`, and `using var
    // live := move w;` adopts it. ONE disposal. Pre-fix the outer source
    // local's scope-end active-case pass walked `W` → `A` and disposed the
    // same block again: ASan heap-use-after-free, plain-build SIGSEGV.
    expectRun(
        run(kNestedDecls,
            "var n := 0; "
            "var o := Outer.W(Inner.A(new Widget())); "
            "case move o of "
            "  W(var i) => do "
            "    case move i of "
            "      A(var w) => do "
            "        using var live := move w; "
            "        n := n + live.Ping(); "
            "      end, "
            "      B => () "
            "    end case; "
            "  end, "
            "  X => () "
            "end case; "
            "Console.WriteLine(\"after\"); "
            "return n;"),
        std::string("widget disposed\nafter\n"), 7);
}

BEZEL_TEST(codegen_owning_match_named_scrutinee_transfer_4609,
           spec_35_5_single_level_named_scrutinee_transfer_disposes_once) {
    // Rule-corpus case s35_5_0003, which is §35.5's own example: "`case move
    // outcome of` — marked consume; `outcome` moved-from after `end`" with the
    // arm's `using var widget := move w;` adoption. It printed all three
    // expected lines and then SIGSEGV'd on the way out, because "moved-from"
    // was never recorded. Nesting is not required for the defect.
    expectRun(
        run(kFlatDecls,
            "var n := 0; "
            "var o := Outer.W(new Widget()); "
            "case move o of "
            "  W(var w) => do "
            "    using var live := move w; "
            "    n := n + live.Ping(); "
            "  end, "
            "  X => () "
            "end case; "
            "Console.WriteLine(\"after\"); "
            "return n;"),
        std::string("widget disposed\nafter\n"), 7);
}

BEZEL_TEST(codegen_owning_match_named_scrutinee_transfer_4609,
           spec_35_5_marked_scrutinee_adoption_in_a_loop_disposes_exactly_n) {
    // The strong oracle. A one-shot double free can land in still-mapped
    // memory and pass on both sides, so amplify: 200 constructs, each
    // producing one Widget, consuming its container with `case move`, and
    // adopting the payload. The count must be exactly 200 — 400 is the
    // pre-fix double free, 0 is a reclaim turned into a leak. Also proves the
    // moved-from zero-store re-arms every iteration rather than latching.
    expectRun(
        run(kCountingFlatDecls,
            "var i := 0; "
            "while i < 200 do "
            "  var o := Outer.W(new Widget()); "
            "  case move o of "
            "    W(var w) => do using var live := move w; end, "
            "    X => () "
            "  end case; "
            "  i := i + 1; "
            "end while; "
            "Console.WriteLine(Widget._n); "
            "return 0;"),
        std::string("200\n"), 0);
}

// --- THE CONTROL: the issue's discriminator, clean on both sides ------------

BEZEL_TEST(codegen_owning_match_named_scrutinee_transfer_4609,
           control_unbound_temporary_scrutinee_was_already_clean) {
    // The issue's discriminating pair: the SAME program with an unbound
    // temporary scrutinee is clean before the fix and after it. #4552 gave
    // that form its construct-completion reclaim and declined it for a binding
    // arm, so the arm's `using var` was already the sole owner. Keeping this
    // row beside the one above is what proves the named binding is the whole
    // difference — and that the fix did not disturb the form that worked.
    expectRun(
        run(kNestedDecls,
            "var n := 0; "
            "case Outer.W(Inner.A(new Widget())) of "
            "  W(var i) => do "
            "    case move i of "
            "      A(var w) => do "
            "        using var live := move w; "
            "        n := n + live.Ping(); "
            "      end, "
            "      B => () "
            "    end case; "
            "  end, "
            "  X => () "
            "end case; "
            "Console.WriteLine(\"after\"); "
            "return n;"),
        std::string("widget disposed\nafter\n"), 7);
}

// --- The reclaim must not become a leak -------------------------------------

BEZEL_TEST(codegen_owning_match_named_scrutinee_transfer_4609,
           spec_35_5_discarded_slot_under_a_marked_scrutinee_still_disposes) {
    // §35.5: "Discarded owned and move-only value slots (`Fetched(_)`), like
    // unlisted ones, are disposed at construct completion". The marked form
    // transfers nothing here, so the payload must still be reclaimed — and, as
    // for the temporary form, at the construct's completion, i.e. BEFORE
    // "after" runs. Pre-fix it was disposed too, but at the source local's
    // SCOPE end (after "after"): the ordering is what makes the boundary
    // observable, and the row guards the marked form against acquiring the
    // leak that a naive "just stop disposing the source" fix would ship.
    expectRun(
        run(kFlatDecls,
            "var o := Outer.W(new Widget()); "
            "case move o of "
            "  W(_) => Console.WriteLine(\"hit\"), "
            "  X    => () "
            "end case; "
            "Console.WriteLine(\"after\"); "
            "return 0;"),
        std::string("hit\nwidget disposed\nafter\n"), 0);
}

BEZEL_TEST(codegen_owning_match_named_scrutinee_transfer_4609,
           spec_35_5_discarding_arm_of_a_mixed_construct_still_disposes) {
    // The per-arm precision this fix buys. §35.5 disposes at completion the
    // payloads "not transferred into bindings" — a property of the SELECTED
    // arm. The construct below binds in the `A` arm and discards in the `B`
    // arm, and `B` is the active case, so the payload transfers nowhere and
    // must be disposed at completion. #4552's whole-construct arms-bind gate
    // would decline the reclaim for every arm of this construct and leak it.
    // (Not a FAIL-WITHOUT row: pre-fix the source local's scope-end pass
    // happened to cover this one. It guards THIS fix's own precision.)
    expectRun(
        run(kTwoCaseDecls,
            "var o := Outer.B(new Widget()); "
            "case move o of "
            "  A(var x) => do using var live := move x; end, "
            "  B(_)     => Console.WriteLine(\"discarded\") "
            "end case; "
            "Console.WriteLine(\"after\"); "
            "return 0;"),
        std::string("discarded\nwidget disposed\nafter\n"), 0);
}

BEZEL_TEST(codegen_owning_match_named_scrutinee_transfer_4609,
           spec_35_5_early_return_out_of_a_discarding_arm_still_disposes) {
    // The edge that decided how the reclaim is registered. §35.5 disposes the
    // un-transferred payloads when the construct completes — a `return` out of
    // an arm never reaches that point, so the dispose-stack entry has to cover
    // it. #4552 registered the entry `unwindOnly`, which `emitDisposals` skips
    // unless the edge is an unwind, and this shape therefore leaked:
    // MEASURED `LeakSanitizer: 16 byte(s) leaked in 1 allocation(s)` on a
    // mid-fix build of #4609, against `hit` / `widget disposed` from the
    // compiler before it (the source local's scope-end pass covered the
    // return). The entry is a plain one now, discharged by the two runtime
    // zero-stores instead of by its registration kind.
    expectRun(
        run(kFlatDecls,
            "var o := Outer.W(new Widget()); "
            "case move o of "
            "  W(_) => do Console.WriteLine(\"hit\"); return 5; end, "
            "  X    => () "
            "end case; "
            "return 0;"),
        std::string("hit\nwidget disposed\n"), 5);
}

BEZEL_TEST(codegen_owning_match_named_scrutinee_transfer_4609,
           spec_35_5_exception_out_of_an_adopting_arm_disposes_once) {
    // §35.5: "An exception out of an arm disposes the not-yet-consumed
    // remainder during unwinding (K3.15)." The remainder, not the transferred
    // payload: the arm's `using var` owns the Widget on the unwind edge too,
    // so exactly one disposal must reach the handler. This is why the fix's
    // per-arm discharge is a runtime zero-store over the captured value rather
    // than a compile-time skip of the normal edge — the unwind entry reads the
    // same zeros.
    expectRun(
        run(kFlatDecls,
            "try "
            "  var o := Outer.W(new Widget()); "
            "  case move o of "
            "    W(var w) => do "
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
        std::string("widget disposed\ncaught\nafter\n"), 0);
}

BEZEL_TEST(codegen_owning_match_named_scrutinee_transfer_4609,
           spec_35_5_unmarked_named_scrutinee_is_still_a_borrowing_match) {
    // §35.5 *The borrowing match*: "An unmarked read of a named owned binding
    // in scrutinee position stays a borrowing match" — the construct consumes
    // nothing and §35.4's scope-end active-case pass remains the owner, so the
    // disposal lands AFTER "after". The fix keys strictly on the `move`
    // marker; this row pins that the unmarked spelling was left alone (it is
    // #4552's own `spec_35_5_named_local_scrutinee_is_not_disposed_by_the_
    // construct` row, restated for the owning-payload shape this fix touches).
    expectRun(
        run(kFlatDecls,
            "var o := Outer.W(new Widget()); "
            "case o of "
            "  W(_) => Console.WriteLine(\"hit\"), "
            "  X    => () "
            "end case; "
            "Console.WriteLine(\"after\"); "
            "return 0;"),
        std::string("hit\nafter\nwidget disposed\n"), 0);
}
