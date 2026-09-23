// §35.5's four named discharges for an owning-match arm binding
// (issue #4734; spec §35.5 *The owning match*, §28.6 "Who may call
// Dispose()", §17.13 *Discard of a named owned binding*, §28.8, v0.307).
//
// THE RULE. §35.5 makes an `owned`-payload binding of an OWNING match "an
// **owned local of the arm body's scope**", to which "§28.3's standard rules
// apply … on every arm-exit edge", and it names the discharges verbatim:
//
//   "falling off the arm still owning the value auto-drops it with the
//    must-use warning naming the binding and the standard remedies
//    (`using var u := move r;` adoption in a `do … end` arm, an
//    owned-consuming call, an explicit `r.Dispose()`, `_ := r;`)"
//
// THE BUG (#4734). Only the FIRST worked. MEASURED on this lane's base
// (afee220e), over §35.5's own shape
// `choice Outer W(owned a: Widget, owned b: Widget), X;` with
// `case move o of W(var w, _) => do <REMEDY> end, X => () end case;`:
//
//   using var u := move w;   →  7 / dispose a / dispose b / after   exit 0
//   w.Dispose();             →  REJECTED: "cannot call `Dispose()` on `w`: a
//                               pattern binding is a non-destructive borrow of
//                               the matched value (§19.3, §28.6 …)"
//   Take(move w)             →  REJECTED: "a borrow cannot be passed where an
//                               `owned` value is required … (§28.8)"
//   _ := w;                  →  ACCEPTED, and discharged NOTHING: stdout was
//                               `dispose b` / `after` and LeakSanitizer
//                               reported 24 bytes in 1 allocation, exit 1.
//
// Each of the three asked a predicate keyed on the binding's DECL kind — an
// arm binding's `decl` is a `BindingPattern` — and answered with §19.3's
// BORROWING-match rule, which §35.5 scopes to the borrowing match alone.
// (§28.6's borrow-receiver list names "a pattern binding (§19.3)" in exactly
// that sense; in an owning match `r.Dispose()` is the owner disposing its own
// value, which the same section admits.) The third row is the silent-degrade
// shape: an ACCEPTED program that leaks.
//
// OBSERVABILITY. Rows 1 and 2's pre-fix behaviour is a REJECT, so they fail
// without the fix at `r.ran == false` with the quoted diagnostic present. Row
// 3's pre-fix behaviour is an ACCEPT, so a compile-only assertion cannot see
// it — the oracle must RUN and read the disposal trace. Every row therefore
// asserts stdout, and each remedy additionally gets a 500-iteration amplified
// row with a counting destructor asserting the EXACT total: 2 disposals per
// iteration, 1000 for 500 iterations. Below it is a leak, above it a double
// free — the two error directions a discharge can introduce, separated at
// once. 500 iterations because a one-shot LeakSanitizer probe can keep its
// block reachable from a live stack slot and read clean on BOTH sides.
//
// per §35.5, §28.6, §17.13, §28.8, §28.3, v0.307

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// §35.5's own owning-choice shape: two `owned` class payloads and a
// payload-free case, plus the `owned`-consuming sink the second remedy names.
const char* kDecls =
    "public class Widget "
    "  private _n: string; "
    "  public constructor(n: string) _n := n; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_n}\"); end destructor; "
    "  public method Ping(): int => 7; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; ";

// The same shape with a COUNTING destructor, for the amplified rows.
const char* kCountingDecls =
    "public class Widget "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Widget._n := Widget._n + 1; end destructor; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; ";

// `Take` is §35.5's "owned-consuming call": it adopts the payload and
// disposes it at its own scope end (§28.10).
std::string prog(const char* decls, const std::string& body) {
    return std::string("module M; ") + decls +
           "public class Program "
           "  public static method Take(owned x: Widget): int "
           "    using var u := owned x; "
           "    return 7; "
           "  end Take; "
           "  public static method Main(): int " + body +
           "  end Main; "
           "end Program;";
}

// The arm shape every row shares: §35.5's second announcing scrutinee form
// (`case move o of …`) over a `do … end` statement arm.
std::string armBody(const std::string& remedy) {
    return "var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
           "case move o of "
           "  W(var w, _) => do " + remedy + " end, "
           "  X => () "
           "end case; "
           "Console.WriteLine(\"after\"); "
           "return 0;";
}

bool hasErrorContaining(const JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

void expectRun(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The two pre-fix diagnostics, asserted ABSENT on their accept rows: each row
// must pass because its remedy is admitted, not because some other error text
// happened to change.
const char* kDisposeNeedle = "a pattern binding is a non-destructive borrow";
const char* kOwnedSinkNeedle =
    "a borrow cannot be passed where an `owned` value is required";

// --- REMEDY 2: an explicit `r.Dispose()` (§35.5, §28.6) ---------------------

BEZEL_TEST(owning_match_arm_discharges_4734,
           spec_35_5_explicit_dispose_of_transferred_binding_admitted) {
    // `dispose a` lands at the `Dispose()` call (§17.13/§28.6: disposal at the
    // disposing statement); `dispose b` is §35.5's un-transferred remainder,
    // reclaimed at construct completion. Both before "after".
    auto r = runAot(prog(kDecls, armBody("w.Dispose();")));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kDisposeNeedle));
    expectRun(r, std::string("dispose a\ndispose b\nafter\n"));
}

BEZEL_TEST(owning_match_arm_discharges_4734,
           spec_35_5_explicit_dispose_naked_binding_spelling_admitted) {
    // The naked `W(w, _)` spelling binds through #1271's synthesized anchor
    // rather than a written `var w`. §35.5 draws no distinction, and the
    // codegen slot is keyed on the same anchor node — so the reclaim must
    // find it here too.
    auto r = runAot(prog(kDecls,
        "var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "case move o of "
        "  W(w, _) => do w.Dispose(); end, "
        "  X => () "
        "end case; "
        "Console.WriteLine(\"after\"); "
        "return 0;"));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kDisposeNeedle));
    expectRun(r, std::string("dispose a\ndispose b\nafter\n"));
}

// --- REMEDY 3: an owned-consuming call (§35.5, §28.8) ----------------------

BEZEL_TEST(owning_match_arm_discharges_4734,
           spec_35_5_owned_consuming_call_of_transferred_binding_admitted) {
    // The statement-position, call-argument sibling of #4633's
    // expression-position arm-result fix. `Take` adopts the payload into its
    // own `using var`, which disposes it at `Take`'s scope end — so
    // `dispose a` lands before `Take` returns 7.
    auto r = runAot(prog(kDecls,
        armBody("Console.WriteLine(Take(move w));")));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kOwnedSinkNeedle));
    expectRun(r, std::string("dispose a\n7\ndispose b\nafter\n"));
}

// --- REMEDY 4: `_ := r;` (§35.5, §17.13) — THE SILENT DEGRADE -------------

BEZEL_TEST(owning_match_arm_discharges_4734,
           spec_17_13_discard_of_transferred_binding_actually_disposes) {
    // §17.13 *Discard of a named owned binding*: "the value is disposed at the
    // end of the discard statement". Pre-fix this program COMPILED and the
    // discard emitted nothing: stdout was "dispose b\nafter\n" and LSan
    // reported 24 bytes leaked. The oracle is the stdout trace, which no
    // compile-only assertion could see.
    auto r = runAot(prog(kDecls, armBody("_ := w;")));
    expectRun(r, std::string("dispose a\ndispose b\nafter\n"));
}

// --- THE AMPLIFIED ORACLES -------------------------------------------------

std::string ampBody(const std::string& remedy) {
    return "var i := 0; "
           "while i < 500 do "
           "  var o := Outer.W(new Widget(), new Widget()); "
           "  case move o of "
           "    W(var w, _) => do " + remedy + " end, "
           "    X => () "
           "  end case; "
           "  i := i + 1; "
           "end while; "
           "Console.WriteLine(Widget._n); "
           "return 0;";
}

BEZEL_TEST(owning_match_arm_discharges_4734,
           spec_35_5_five_hundred_explicit_disposes_count_exactly_2n) {
    // 1000 is the EXACT answer: per iteration the transferred payload is
    // disposed once by `w.Dispose()` and the un-transferred sibling once at
    // construct completion. Below it is a leak, above it a double free.
    auto r = runAot(prog(kCountingDecls, ampBody("w.Dispose();")));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1000\n"));
}

BEZEL_TEST(owning_match_arm_discharges_4734,
           spec_35_5_five_hundred_owned_consuming_calls_count_exactly_2n) {
    auto r = runAot(prog(kCountingDecls, ampBody("_ := Take(move w);")));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1000\n"));
}

BEZEL_TEST(owning_match_arm_discharges_4734,
           spec_17_13_five_hundred_discards_count_exactly_2n) {
    // The loop-amplified form of the silent-degrade row. Pre-fix this prints
    // 500 (only the un-transferred sibling is reclaimed) — the leak the
    // one-shot LSan row can miss when the block stays reachable.
    auto r = runAot(prog(kCountingDecls, ampBody("_ := w;")));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1000\n"));
}

// --- THE SECOND-CONSUME SAFETY NET -----------------------------------------

BEZEL_TEST(owning_match_arm_discharges_4734,
           spec_28_8_1_move_then_dispose_is_not_a_double_free) {
    // §28.8.1: "a moved-from binding is unreadable until reassigned" — but the
    // dataflow keeps NO `states_` entry for a pattern binding, so it cannot
    // REJECT this program the way it rejects the same shape on an ordinary
    // owned local, and admitting `r.Dispose()` without more would have made it
    // free a block the adopting `using var` also owns. The `move` now nulls
    // the binding's slot, so the later reclaim reloads null and skips: the
    // payload is disposed exactly ONCE, by `u`. (Diagnosing the second consume
    // needs the `states_` entry #4634 introduces; this row pins the memory
    // safety, not the diagnostic.)
    auto r = runAot(prog(kDecls, armBody(
        "using var u := move w; w.Dispose();")));
    expectRun(r, std::string("dispose a\ndispose b\nafter\n"));
}

BEZEL_TEST(owning_match_arm_discharges_4734,
           spec_28_8_1_move_then_discard_is_not_a_double_free) {
    // The `_ := r;` twin of the row above. Pre-#4734 the discard emitted
    // nothing so this shape was accidentally safe; making the discard REAL is
    // what created the hazard the slot-null closes.
    auto r = runAot(prog(kDecls, armBody("using var u := move w; _ := w;")));
    expectRun(r, std::string("dispose a\ndispose b\nafter\n"));
}

BEZEL_TEST(owning_match_arm_discharges_4734,
           spec_28_8_1_five_hundred_move_then_dispose_count_exactly_2n) {
    // The amplified form: still exactly 2 disposals per iteration. A
    // double free would abort (ran == false) and a leak would undercount.
    auto r = runAot(prog(kCountingDecls,
                         ampBody("using var u := move w; w.Dispose();")));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1000\n"));
}

// --- THE CONTROLS: the BORROWING match must be untouched -------------------

BEZEL_TEST(owning_match_arm_discharges_4734,
           control_borrowing_match_dispose_still_rejected_19_3) {
    // §35.5 *The borrowing match*: an UNMARKED named owned scrutinee stays a
    // borrowing match, whose bindings are ordinary BORROWS of a value `o`
    // still owns. §28.6's "a pattern binding (§19.3)" borrow-receiver row
    // applies with full force here; disposing would free a value `o`'s
    // scope-end pass disposes too.
    auto r = runAot(prog(kDecls,
        "using var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "case o of "
        "  W(var w, _) => do w.Dispose(); end, "
        "  X => () "
        "end case; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kDisposeNeedle));
}

BEZEL_TEST(owning_match_arm_discharges_4734,
           control_borrowing_match_owned_consuming_call_still_rejected_28_8) {
    auto r = runAot(prog(kDecls,
        "using var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "case o of "
        "  W(var w, _) => do _ := Take(move w); end, "
        "  X => () "
        "end case; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kOwnedSinkNeedle));
}

BEZEL_TEST(owning_match_arm_discharges_4734,
           control_borrowing_match_discard_disposes_nothing_17_13) {
    // §17.13's borrow carve-out: "a discard whose operand is in borrow state …
    // disposes nothing … and the value's owner keeps the obligation". Both
    // payloads must still be disposed by `o`'s own scope end, AFTER "after" —
    // exactly once each, which the admission must not disturb.
    auto r = runAot(prog(kDecls,
        "using var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "case o of "
        "  W(var w, _) => do _ := w; end, "
        "  X => () "
        "end case; "
        "Console.WriteLine(\"after\"); "
        "return 0;"));
    expectRun(r, std::string("after\ndispose a\ndispose b\n"));
}

BEZEL_TEST(owning_match_arm_discharges_4734,
           control_discarded_sibling_slot_still_reclaimed_at_completion_35_5) {
    // §35.5's un-transferred remainder, isolated: an arm that binds NOTHING
    // takes no transfer at all, so BOTH payloads are disposed at construct
    // completion. The admission keys on the transferred slot only; a
    // non-binding arm must be unaffected.
    auto r = runAot(prog(kDecls,
        "var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "case move o of "
        "  W(_, _) => do Console.WriteLine(\"arm\"); end, "
        "  X => () "
        "end case; "
        "Console.WriteLine(\"after\"); "
        "return 0;"));
    expectRun(r, std::string("arm\ndispose a\ndispose b\nafter\n"));
}

}  // namespace
