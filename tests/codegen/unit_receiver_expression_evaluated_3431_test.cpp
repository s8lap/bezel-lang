// fix #3431 — a UNIT-VALUED RECEIVER EXPRESSION was never evaluated, so
// `C.SomeUnitMethod().ToString()` silently DELETED the call from the program
// (spec §7.5 *Null-conditional access (`?.`)*, §22.3 *Parameter modifiers*,
// §22.5 *Optional parameters and named arguments*, §8.2 *`GetType`*,
// §8.5 *`ToString`*, §4.7 *The unit type* — v0.283).
//
// MEASURED on 1e3861a (this file's parent commit), with the issue's repro:
//
//     Console.WriteLine("A");
//     var s := C.Nothing().ToString();      // C.Nothing() prints "side"
//     Console.WriteLine("B" + s);
//
//   stdout:  A
//            B()
//   exit:    0
//
// `"side"` never printed. The `.ToString()` answer was still CORRECT — `"()"`,
// exactly what §8.5 specifies — which is what made the defect silent: the value
// is right and the side effect is gone. The same shape through a LOCAL
// (`var u := C.Nothing(); u.ToString()`) printed "side" correctly, so the axis
// is specifically "the receiver is a unit-valued EXPRESSION".
//
// ── WHY IT IS A DEFECT: A RECEIVER IS AN EVALUATED OPERAND ───────────────────
//
// The §8.5 answer for unit is a compile-time constant, and codegen's unit-
// `ToString` arm concluded from that that it need not lower the receiver at
// all. Those are two different claims. "Nothing about the receiver's VALUE
// needs to be read" is true; "the receiver EXPRESSION needs no evaluating" does
// not follow — the value may be uninteresting but the expression that produces
// it can do arbitrary work, here I/O. Four rules say so, and none of them
// carves out a receiver whose type has one value:
//
//   * §7.5 *Evaluation*: "`?.` short-circuits. The receiver is evaluated
//     first; if it is null the access does not occur and **nothing to its right
//     in the chain is evaluated**". A rule that a NULL receiver SUPPRESSES the
//     rest presupposes that a non-null receiver's expression RUNS — and `.` has
//     no short-circuit at all.
//   * §22.3, the `ref`/`out` argument-locations passage, states it outright for
//     the compound/indexer path: "the receiver and every index expression are
//     evaluated exactly once, in §22.5's call-site textual order." Exactly
//     once — not zero times. (Quoted plain. The spec emphasises neither this
//     phrase nor §8.2's below; an earlier draft of this comment bolded both,
//     which reads as the document stressing them. It does not.)
//   * §22.5 *Evaluation order*: "The order is observable through side-effecting
//     arguments and non-constant defaults (`DateTime.Now`, factory calls) and
//     is normative…" (the spec sentence continues "; it matches C#'s
//     treatment of supplied arguments" — truncated here, not ended)
//   * §8.2 is the closest thing in the document to a licence for answering a
//     universal from the static type, and it is explicit about what that
//     removes: "the call is answered from the static type with no observable
//     boxing". It removes the BOX, not the receiver.
//
// So the fix is one line at the one site that had the licence backwards: lower
// the receiver for its effects, exactly once, and THEN answer §8.5's constant.
//
// ── WHAT MAKES EACH CASE ABLE TO FAIL ────────────────────────────────────────
//
// The pre-fix program EXITS 0 and prints a CORRECT `"()"`, so neither "it ran"
// nor "the rendering is right" is an oracle here — both hold on the broken
// compiler. Every case below therefore asserts the EXACT stdout including the
// receiver's own marker line, and the multi-call cases interleave numbered
// markers so a fix that lowered the receiver the wrong number of times, or in
// the wrong order, fails as loudly as one that did not lower it at all.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// `Nothing` is declared with NO return type — §4.7's "It is the return type of
// methods declared without an explicit return type" — so `C.Nothing()` is a
// unit-valued expression that also performs observable I/O. `Say` is the same
// thing with the marker chosen by the caller, for the ordering cases.
constexpr const char* kDecls =
    "module M3431; "
    "class C "
    "  public static method Nothing() "
    "    Console.WriteLine(\"side\"); "
    "  end Nothing; "
    "  public static method Say(s: string) "
    "    Console.WriteLine(s); "
    "  end Say; ";

std::string prog(const std::string& body) {
    return std::string(kDecls) +
           "  public static method Main(): int " + body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ── T1 — THE ISSUE'S EXACT REPRO. Pre-fix: "A\nB()\n" (exit 0, no diagnostic).
// Post-fix the receiver's own `Console.WriteLine` appears between them, and
// `s` still holds §8.5's `"()"` — the fix must ADD the effect, not trade the
// rendering for it.
BEZEL_TEST(codegen_unit_receiver_expression_evaluated_3431,
           unit_valued_receiver_call_is_evaluated_spec_22_3_8_5) {
    auto r = runAot(prog(
        "    Console.WriteLine(\"A\"); "
        "    var s := C.Nothing().ToString(); "
        "    Console.WriteLine(\"B\" + s); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("A\nside\nB()\n"));
}

// ── T2 — EXACTLY ONCE, IN ORDER (§22.3's "evaluated exactly once", §22.5's
// "call-site textual order"). Three unit-valued receivers with distinct markers,
// interleaved with numbered statements. A receiver lowered twice, lowered
// out of order, or hoisted out of the statement it belongs to all produce a
// different string here; only "once, in place" produces this one.
BEZEL_TEST(codegen_unit_receiver_expression_evaluated_3431,
           unit_receiver_evaluated_exactly_once_in_order_spec_22_3_22_5) {
    auto r = runAot(prog(
        "    var a := C.Say(\"r1\").ToString(); "
        "    Console.WriteLine(\"1\" + a); "
        "    var b := C.Say(\"r2\").ToString(); "
        "    Console.WriteLine(\"2\" + b); "
        "    var c := C.Say(\"r3\").ToString(); "
        "    Console.WriteLine(\"3\" + c); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("r1\n1()\nr2\n2()\nr3\n3()\n"));
}

// ── T3 — the EXPRESSION-STATEMENT form, where the `"()"` result is discarded
// and the receiver's effect is the only thing the statement can be for. Pre-fix
// this emitted NOTHING AT ALL between the markers — the entire statement was a
// no-op with no diagnostic at any severity.
BEZEL_TEST(codegen_unit_receiver_expression_evaluated_3431,
           unit_receiver_in_discarded_expression_statement_spec_22_3) {
    auto r = runAot(prog(
        "    Console.WriteLine(\"A\"); "
        "    C.Nothing().ToString(); "
        "    Console.WriteLine(\"B\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("A\nside\nB\n"));
}

// ── T4 — the ARGUMENT position, with TWO unit-valued receivers in one call, so
// §22.5's "call-site textual order … is normative" is pinned and not merely the
// count. Pre-fix stdout was "()()\n" alone; both receiver markers were gone.
BEZEL_TEST(codegen_unit_receiver_expression_evaluated_3431,
           two_unit_receivers_in_one_call_run_in_textual_order_spec_22_5) {
    auto r = runAot(prog(
        "    Console.WriteLine(C.Say(\"L\").ToString() + "
        "                      C.Say(\"R\").ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("L\nR\n()()\n"));
}

// ── T5 — the INTERPOLATION-HOLE form (§18.5). A different consumer of the same
// lowering, reached through a different path in codegen, and broken identically
// pre-fix: `[()]` with no "side".
BEZEL_TEST(codegen_unit_receiver_expression_evaluated_3431,
           unit_receiver_inside_interpolated_string_spec_18_5_22_3) {
    auto r = runAot(prog(
        "    Console.WriteLine($\"[{C.Nothing().ToString()}]\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\n[()]\n"));
}

// ── T6 — the result is REALLY §8.5's two-character string, not merely something
// that prints like it. Chaining `.Length` off the answer would read a null
// pointer if the fix had produced the effect at the cost of the value.
BEZEL_TEST(codegen_unit_receiver_expression_evaluated_3431,
           unit_receiver_tostring_result_is_the_spec_8_5_string) {
    auto r = runAot(prog(
        "    var n := C.Nothing().ToString().Length; "
        "    Console.WriteLine(n.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\n2\n"));
}

// ── T7 — CONTROL, the §30.3 STDLIB receiver. §30.3 declares `public method
// Wait();` with no return type, which §4.7 makes unit, so `t.Wait().ToString()`
// is this issue's shape with a stdlib callee instead of a user one. MEASURED on
// the parent commit: this row prints "()\n14\n" BEFORE the fix as well — the
// deleted `Wait()` is not observable here because `t.Result` joins on its own
// — so it is a control on the stdlib spelling and NOT one of the failing rows.
// It is kept because the fix newly emits a `Wait()` call on this path, and a
// double join or a join emitted at the wrong point would show up here as a hang
// or a wrong `Result`. (§30.3 names `Result`, not `Wait()`'s return, as where a
// task's value is read — the #3391 ruling.)
BEZEL_TEST(codegen_unit_receiver_expression_evaluated_3431,
           task_wait_as_unit_receiver_still_joins_spec_30_3_4_7) {
    auto r = runAot(
        "module M3431B; "
        "class C "
        "  private static async method Work(i: int): Task<int> "
        "    await Task.Delay(1); "
        "    return i * 2; "
        "  end Work; "
        "  public static method Main(): int "
        "    using var t := C.Work(7); "
        "    Console.WriteLine(t.Wait().ToString()); "
        "    Console.WriteLine(t.Result.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("()\n14\n"));
}

// ── T8 — CONTROL: the shapes that ALREADY worked and must be byte-identical
// after the fix. The unit LITERAL receiver has no effect to run, a unit LOCAL's
// receiver is a slot read, and the local form already printed "side" at its
// declaration. If the fix had lowered a receiver twice, the local case would
// print "side" twice here.
BEZEL_TEST(codegen_unit_receiver_expression_evaluated_3431,
           literal_and_local_unit_receivers_unchanged_spec_4_7_8_5) {
    auto r = runAot(prog(
        "    Console.WriteLine(().ToString()); "
        "    var u: () := (); "
        "    Console.WriteLine(u.ToString()); "
        "    var v := C.Nothing(); "
        "    Console.WriteLine(v.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("()\n()\nside\n()\n"));
}

// ── T9 — CONTROL on the `()?` spellings, which reach the unit-`ToString` arm
// through the null-forgiving `!` and bypass it entirely through `?.` (§7.5's
// own node). §4.7: "`()?` … produces an optional with two values, `null` and
// `()`", and §7.5 says a null receiver skips the access — so the `null` arm
// must still produce no call and no rendering.
BEZEL_TEST(codegen_unit_receiver_expression_evaluated_3431,
           nullable_unit_receiver_forms_unchanged_spec_4_7_7_5) {
    auto r = runAot(prog(
        "    var p: ()? := (); "
        "    Console.WriteLine(p!.ToString()); "
        "    var z: ()? := null; "
        "    Console.WriteLine(z?.ToString() ?? \"NULL\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("()\nNULL\n"));
}
