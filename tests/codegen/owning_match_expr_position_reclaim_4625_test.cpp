// Codegen regression tests for issue #4625: §28.3 *`case` scrutinees extend the
// boundary* — an owning match in EXPRESSION position had no payload reclaim at
// all (spec v0.307).
//
// THE BUG. §28.3 states the K8.3 boundary for a `case` whose scrutinee produces
// an unbound owned temporary as "the **whole `case` construct**, in statement
// position exactly as in expression position". `lowerCaseStmt` implements that
// for an owning-CHOICE scrutinee (#4552 added the capture, #4609 the per-arm
// discharge, #4617 the per-slot one). `lowerCaseExpr` implemented NONE of it —
// no captured slot, no dispose-stack entry, no construct-completion reclaim —
// so an owning match in expression position leaked its payloads outright. This
// is a missing mechanism, not the granularity defect #4617 fixed.
//
// MEASURED on this lane's HEAD before the fix, over
// `choice Outer W(owned a: Widget, owned b: Widget), X;`:
//
//     var r := (case Outer.W(new Widget("a"), new Widget("b")) of
//               W(_, _) => 1, X => 2);
//
//     r=1 / after                       ← neither destructor ran
//     LeakSanitizer: 48 byte(s) leaked in 2 allocation(s)
//
// and, amplified to 200 constructs with a counting destructor, 0 disposals
// where §35.5 requires 400. §35.5's OTHER announcing scrutinee form,
// `case move o of …`, did not leak but disposed at the WRONG TIME — the source
// local was never left moved-from, so its scope-end active-case pass disposed
// the payloads at end of method instead of at construct completion.
//
// ⚠ THE SPELLING. The expression form of `case` is the §19.1 (v0.208)
// parenthesized atom — `(case s of A => e, B => e)`, closed by the paren, with
// no `end`. The `case … end case` spelling is statement position only; used in
// expression position it is a *parse* error, so a "repro" written that way
// measures nothing. Every row below uses the atom.
//
// THE ORDERING CONSTRAINT expression position adds over statement position: the
// reclaim must run AFTER the selected arm's result is computed (or it frees a
// payload the result still borrows) and CANNOT precede the result PHI (a PHI
// must be the first instruction of its block, and the reclaim emits a tag
// switch that terminates its block). The fix builds the PHI first, spills it to
// an entry slot, emits the reclaim, and reloads — so a row whose result value
// is derived from a payload pins both halves: that is
// `spec_35_5_reclaim_runs_after_an_arm_result_derived_from_a_payload` below.
// (It used to be the borrow-escape row, whose program fix #4635 now rejects
// outright — K8.4 forbids a borrow of a bound payload escaping the construct.)
//
// THE ORACLE is the exact disposal TEXT and ORDER, never "it did not crash": a
// printing destructor gives one line per disposal, and the position of those
// lines relative to `r=` and `after` is what distinguishes "reclaimed at
// construct completion" (this fix) from "reclaimed at method scope end" (the
// pre-fix `move` form) from "never reclaimed" (the pre-fix temporary form). The
// amplified rows use a counting destructor over 200/500 iterations rather than
// a one-shot LSan probe, because a single leaked allocation can stay reachable
// from a live stack slot and read clean on BOTH sides of the fix. `r.ran` is
// asserted on every row because the error direction this fix must NOT introduce
// — a double free — is a signal death that `runAot` reports as `ran == false`.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Two `owned` payloads on one case, with a printing destructor so a row pins
// WHICH payload was reclaimed and WHEN.
const char* kTwoPayloadDecls =
    "public class Widget "
    "  private _n: string; "
    "  public constructor(n: string) _n := n; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_n}\"); end destructor; "
    "  public method Ping(): int => 7; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; ";

// The same choice reached through an owning-choice-returning call — §28.3's
// "unbound owned temporary … the result of an `owned`-returning call".
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

// A COUNTING destructor for the amplified rows: an exact count over many
// iterations is the only oracle that separates "each payload disposed once"
// from both "payloads leaked" and "some payload disposed twice".
const char* kCountingTwoPayloadDecls =
    "public class Widget "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Widget._n := Widget._n + 1; end destructor; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; ";

bezel::test::JitResult run(const std::string& decls, const std::string& body) {
    return runAot(std::string("module M; ") + decls +
                  " public class Program public static method Main(): int " +
                  body + " end Main; end Program;");
}

// The same program under generated-code ASan/LSan. `runAotChecked` flips `ran`
// to false on a leak, a double free or a use-after-free, so it is the direct
// oracle for BOTH error directions of this fix.
bezel::test::JitResult runChecked(const std::string& decls,
                                  const std::string& body) {
    return runAotChecked(
        std::string("module M; ") + decls +
        " public class Program public static method Main(): int " + body +
        " end Main; end Program;");
}

void expectRun(const bezel::test::JitResult& r, const std::string& out,
               int exitCode) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// fix #4635 (§28.3 K8.4, v0.307): a row whose program is now REJECTED asserts
// the SPECIFIC diagnostic. `r.ran == false` alone would not do — runAot
// synthesizes an Error diagnostic for a runtime crash too (tests/README.md),
// so the text is what distinguishes the intended rejection.
bool rejectsWith(const bezel::test::JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// --- THE BUG: an owning match in expression position -------------------------

BEZEL_TEST(codegen_owning_match_expr_position_reclaim_4625,
           spec_28_3_unbound_owning_choice_temporary_reclaimed_at_completion) {
    // The issue's repro, in the §19.1 parenthesized-atom spelling. §28.3 puts
    // the boundary at the whole construct, so both payloads are disposed after
    // the selected arm finishes and BEFORE the enclosing statement's value is
    // used. Pre-fix output was `r=1 / after` with neither destructor run and
    // 48 bytes leaked in 2 allocations.
    expectRun(
        run(kTwoPayloadDecls,
            "var r := (case Outer.W(new Widget(\"a\"), new Widget(\"b\")) of "
            "  W(_, _) => 1, "
            "  X       => 2); "
            "Console.WriteLine($\"r={r}\"); "
            "Console.WriteLine(\"after\"); "
            "return 0;"),
        std::string("dispose a\ndispose b\nr=1\nafter\n"), 0);
}

BEZEL_TEST(codegen_owning_match_expr_position_reclaim_4625,
           spec_28_3_owned_returning_call_scrutinee_reclaimed_at_completion) {
    // The same boundary reached through §28.3's other producer spelling — "the
    // result of an `owned`-returning call … used only by borrowing operations
    // within the expression". Pre-fix: `r=1 / after`, both payloads leaked.
    expectRun(
        run(kProducerDecls,
            "using var c := new Client(); "
            "var r := (case c.Fetch() of "
            "  W(_, _) => 1, "
            "  X       => 2); "
            "Console.WriteLine($\"r={r}\"); "
            "Console.WriteLine(\"after\"); "
            "return 0;"),
        std::string("dispose a\ndispose b\nr=1\nafter\n"), 0);
}

BEZEL_TEST(codegen_owning_match_expr_position_reclaim_4625,
           spec_35_5_marked_move_scrutinee_reclaims_at_completion_not_at_scope_end) {
    // §35.5's SECOND announcing scrutinee form in expression position: "a named
    // owned binding under an explicit scrutinee transfer marker". §35.5 makes
    // it a consume with a stated end state — "the consumed binding is left
    // moved-from when the construct completes" — so the payloads are reclaimed
    // by the construct, not by the source local's scope-end pass. Pre-fix this
    // form did not leak but printed `r=1 / after / dispose a / dispose b`: the
    // source was never marked, so the disposals landed at end of `Main`, past
    // the boundary §28.3 states. The ordering of the four lines IS the oracle.
    expectRun(
        run(kTwoPayloadDecls,
            "var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
            "var r := (case move o of "
            "  W(_, _) => 1, "
            "  X       => 2); "
            "Console.WriteLine($\"r={r}\"); "
            "Console.WriteLine(\"after\"); "
            "return 0;"),
        std::string("dispose a\ndispose b\nr=1\nafter\n"), 0);
}

BEZEL_TEST(codegen_owning_match_expr_position_reclaim_4625,
           spec_28_3_void_result_expression_case_reclaims_at_completion) {
    // A unit/void-result case expression (§19.7) forms no result PHI and takes
    // `lowerCaseExpr`'s separate early-return path, so it needs its own reclaim
    // site; without one this shape kept leaking after the PHI path was fixed.
    // Pre-fix: `hit / after`, both payloads leaked.
    expectRun(
        run(kTwoPayloadDecls,
            "(case Outer.W(new Widget(\"a\"), new Widget(\"b\")) of "
            "  W(_, _) => Console.WriteLine(\"hit\"), "
            "  X       => ()); "
            "Console.WriteLine(\"after\"); "
            "return 0;"),
        std::string("hit\ndispose a\ndispose b\nafter\n"), 0);
}

BEZEL_TEST(codegen_owning_match_expr_position_reclaim_4625,
           spec_35_5_throw_out_of_an_expression_arm_disposes_the_payloads) {
    // §35.5: "An exception out of an arm disposes the not-yet-consumed
    // remainder during unwinding (K3.15)". The unwind edge is served by the
    // dispose-stack entry the capture registers, which is why the entry is a
    // plain (not `unwindOnly`) one. Pre-fix there was no entry at all and the
    // payloads leaked through the `catch`.
    expectRun(
        run(kTwoPayloadDecls,
            "try "
            "  var r := (case Outer.W(new Widget(\"a\"), new Widget(\"b\")) of "
            "    W(_, _) => throw new InvalidOperationException(\"boom\"), "
            "    X       => 2); "
            "  Console.WriteLine($\"r={r}\"); "
            "catch "
            "  e: InvalidOperationException => Console.WriteLine(\"caught\") "
            "end try; "
            "Console.WriteLine(\"after\"); "
            "return 0;"),
        std::string("dispose a\ndispose b\ncaught\nafter\n"), 0);
}

// --- THE AMPLIFIED ORACLES ---------------------------------------------------

BEZEL_TEST(codegen_owning_match_expr_position_reclaim_4625,
           spec_28_3_two_hundred_expression_constructs_dispose_exactly_2n) {
    // The strong oracle. 200 constructs, two Widgets each: 400 is §28.3's
    // answer, 0 is the pre-fix leak, and anything above 400 is a double free.
    // The accumulator keeps the case's VALUE observed, so the row also pins
    // that threading the result through the reclaim's spill slot preserves it.
    expectRun(
        run(kCountingTwoPayloadDecls,
            "var i := 0; "
            "var acc := 0; "
            "while i < 200 do "
            "  acc := acc + (case Outer.W(new Widget(), new Widget()) of "
            "    W(_, _) => 1, X => 2); "
            "  i := i + 1; "
            "end while; "
            "Console.WriteLine(Widget._n); "
            "Console.WriteLine(acc); "
            "return 0;"),
        std::string("400\n200\n"), 0);
}

BEZEL_TEST(codegen_owning_match_expr_position_reclaim_4625,
           spec_28_3_five_hundred_construct_loop_is_lsan_clean_under_asan) {
    // LSan only reports UNREACHABLE allocations, so a one-shot repro can keep
    // its block reachable from a live stack slot and read clean on BOTH sides
    // of the fix — amplify. Pre-fix this leaves 1000 unreachable Widgets
    // behind; post-fix none, and the printed count is the second half of the
    // oracle.
    auto r = runChecked(
        kCountingTwoPayloadDecls,
        "var i := 0; "
        "var acc := 0; "
        "while i < 500 do "
        "  acc := acc + (case Outer.W(new Widget(), new Widget()) of "
        "    W(_, _) => 1, X => 2); "
        "  i := i + 1; "
        "end while; "
        "Console.WriteLine(Widget._n); "
        "Console.WriteLine(acc); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1000\n500\n"));
}

BEZEL_TEST(codegen_owning_match_expr_position_reclaim_4625,
           control_marked_move_scrutinee_loop_is_asan_clean_no_double_free) {
    // The OTHER error direction, on the form that can produce it. `case move o
    // of` now captures the value into a construct-owned temporary AND marks the
    // source local moved-from; if the source mark were omitted, `o`'s scope-end
    // active-case pass would dispose the same payloads the construct reclaim
    // already freed — ASan reports that as `double-free` /
    // `heap-use-after-free`, i.e. `ran == false`. 500 iterations so a stale
    // pointer cannot stay in still-mapped memory.
    auto r = runChecked(
        kCountingTwoPayloadDecls,
        "var i := 0; "
        "var acc := 0; "
        "while i < 500 do "
        "  var o := Outer.W(new Widget(), new Widget()); "
        "  acc := acc + (case move o of W(_, _) => 1, X => 2); "
        "  i := i + 1; "
        "end while; "
        "Console.WriteLine(Widget._n); "
        "Console.WriteLine(acc); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1000\n500\n"));
}

// --- THE CONTROLS ------------------------------------------------------------

BEZEL_TEST(codegen_owning_match_expr_position_reclaim_4625,
           control_unmarked_named_scrutinee_is_still_a_borrowing_match) {
    // §35.5 *The borrowing match*: "An unmarked read of a named owned binding
    // in scrutinee position stays a borrowing match" — the construct consumes
    // nothing and §35.4's scope-end active-case pass remains the owner, so both
    // disposals land AFTER "after". The fix must touch only the two announcing
    // forms; this row pins that the borrowing spelling was left alone.
    expectRun(
        run(kTwoPayloadDecls,
            "var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
            "var n := (case o of "
            "  W(var w, _) => w.Ping(), "
            "  X           => 2); "
            "Console.WriteLine($\"n={n}\"); "
            "Console.WriteLine(\"after\"); "
            "return 0;"),
        std::string("n=7\nafter\ndispose a\ndispose b\n"), 0);
}

BEZEL_TEST(codegen_owning_match_expr_position_reclaim_4625,
           control_payload_free_case_selection_reclaims_nothing) {
    // The reclaim reads the runtime TAG and walks only the ACTIVE case (§35.4),
    // so selecting the payload-free `X` must dispose nothing at all — not a
    // mis-decoded `W` payload, which would be a wild free rather than a leak.
    expectRun(
        run(kTwoPayloadDecls,
            "var o := Outer.X; "
            "var r := (case move o of "
            "  W(_, _) => 1, "
            "  X       => 2); "
            "Console.WriteLine($\"r={r}\"); "
            "Console.WriteLine(\"after\"); "
            "return 0;"),
        std::string("r=2\nafter\n"), 0);
}

BEZEL_TEST(codegen_owning_match_expr_position_reclaim_4625,
           control_arm_result_borrowing_a_bound_payload_is_rejected_4635) {
    // ⚠ FLIPPED BY fix #4635 (§28.3 K8.4, v0.307). This row USED to assert
    // that the program below compiles and prints
    // `dispose b / got=7 / after / dispose other` — i.e. that the per-slot
    // discharge keeps payload `a` alive under an arm result that BORROWS it:
    //
    //     var got := (case move o of W(var w, _) => w, X => other);
    //     Console.WriteLine($"got={got.Ping()}");
    //
    // The checker admitted that, and it should never have: `case move o`
    // consumes `o`, an UNMARKED named owned binding written as an arm is a
    // BORROW arm (§28.3), and K8.4 rejects a borrow that would escape its
    // boundary "never lifetime-extended". #4635 closes the hole, so the
    // program is now a COMPILE ERROR and this row asserts the diagnostic.
    //
    // The per-slot discharge stays load-bearing for the shapes that remain
    // admitted — the statement-position adoption (#4617's own rows) and the
    // value-result row below, whose sibling `b` must still be reclaimed at
    // construct completion while the bound slot `a` is not.
    auto r = run(kTwoPayloadDecls,
                 "using var other := new Widget(\"other\"); "
                 "var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
                 "var got := (case move o of "
                 "  W(var w, _) => w, "
                 "  X           => other); "
                 "Console.WriteLine($\"got={got.Ping()}\"); "
                 "Console.WriteLine(\"after\"); "
                 "return 0;");
    BEZEL_EXPECT_TRUE(rejectsWith(r, "borrows owning-match payload binding"));
}

BEZEL_TEST(codegen_owning_match_expr_position_reclaim_4625,
           spec_35_5_reclaim_runs_after_an_arm_result_derived_from_a_payload) {
    // The ORDERING half of the fix, on a shape #4635 leaves admitted: the arm
    // result is a VALUE computed FROM the bound payload (§35.5: "Copyable
    // value payloads bind as copies"), so the result PHI is built from a read
    // of payload `a` and the construct-completion reclaim must run AFTER it.
    // A reclaim emitted before the PHI would free `a` under `w.Ping()`; a
    // reclaim that could not follow the PHI at all is the LLVM constraint the
    // fix's spill/reload works around.
    //
    // `b` is disposed at construct completion; `a` is marked out of the
    // reclaim by #4617's per-slot discharge and is the sibling
    // unadopted-binding leak (#4634) — which is why this row is `runAot`, not
    // `runAotChecked`, exactly as the row it replaces was.
    expectRun(
        run(kTwoPayloadDecls,
            "var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
            "var n := (case move o of "
            "  W(var w, _) => w.Ping(), "
            "  X           => 0); "
            "Console.WriteLine($\"n={n}\"); "
            "Console.WriteLine(\"after\"); "
            "return 0;"),
        std::string("dispose b\nn=7\nafter\n"), 0);
}
