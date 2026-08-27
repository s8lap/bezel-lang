// fix #3445 — an INLINE-ARRAY-VALUED RECEIVER EXPRESSION was never evaluated,
// so `C.MakeArray().ToString()` silently DELETED the call from the program
// (spec §7.5 *Null-conditional access (`?.`)*, §22.3 *Parameter modifiers*,
// §22.5 *Optional parameters and named arguments*, §8.2 *`GetType`*,
// §8.5 *`ToString`*, §15.1 *Inline arrays* — v0.283).
//
// MEASURED on 0c5ecd1 (this file's parent commit), with the issue's repro:
//
//     Console.WriteLine("A");
//     Console.WriteLine(C.Make().ToString());   // C.Make() prints "side-arr"
//     Console.WriteLine("B");
//
//   stdout:  A
//            [3]int
//            B
//   exit:    0
//
// `"side-arr"` never printed. The `.ToString()` answer was still CORRECT —
// `"[3]int"`, exactly what §8.5 specifies for an inline array — which is what
// made the defect silent: the value is right and the side effect is gone.
//
// This is fix #3431's defect one type over: that one is the `()` receiver at
// `lowerCallDispatch`'s unit-`ToString` arm, this one is the `[N]T` receiver at
// `tryLowerInlineArrayCall`'s §8.5 `ToString` arm. Those two are the WHOLE blast
// radius. Measured on the parent commit with a side-effecting producer, one
// receiver kind per program: `[3]int` NO, `()` NO (#3431), positional record
// yes, `int` yes, `owned Array<int>` yes. The two failing rows are exactly the
// two whose §8.5 answer is a compile-time constant that needs no receiver value
// — everything that has to READ the receiver lowers it as a side effect of
// needing it. Within `tryLowerInlineArrayCall` itself the other four members
// (`ToArray`, `Contains`, `Equals`, `GetHashCode`) all fall through to the
// `arrAddr` computation and so were never affected; T7 pins that.
//
// ── WHY IT IS A DEFECT: A RECEIVER IS AN EVALUATED OPERAND ───────────────────
//
// The §8.5 answer for an inline array is a compile-time constant (N and the
// element type are both static), and the arm concluded from that that it need
// not lower the receiver at all. Those are two different claims. "Nothing about
// the receiver's VALUE needs to be read" is true — the arm's own comment says
// so — but "the receiver EXPRESSION needs no evaluating" does not follow: the
// value may be uninteresting while the expression that produces it does
// arbitrary work, here I/O. Four rules say a receiver is evaluated, and none of
// them carves out a receiver whose §8.5 answer happens to be static:
//
//   * §7.5 *Evaluation*: "`?.` short-circuits. The receiver is evaluated
//     first; if it is null the access does not occur and **nothing to its
//     right in the chain is evaluated**". A rule that a NULL receiver SUPPRESSES the
//     rest presupposes that a non-null receiver's expression RUNS — and `.` has
//     no short-circuit at all.
//   * §22.3, the `ref`/`out` argument-locations passage, states it outright for
//     the compound/indexer path: "the receiver and every index expression are
//     evaluated exactly once, in §22.5's call-site textual order." Exactly
//     once — not zero times. (Quoted plain; the spec emphasises neither this
//     phrase nor §8.2's below.)
//   * §22.5 *Evaluation order*: "The order is observable through side-effecting
//     arguments and non-constant defaults (`DateTime.Now`, factory calls) and
//     is normative …" — the trailing ellipsis is load-bearing: the sentence
//     continues "; it matches C#'s treatment of supplied arguments", so a
//     period there would present a truncation as the whole sentence.
//   * §8.2 is the closest thing in the document to a licence for answering a
//     universal from the static type, and it is explicit about what that
//     removes: "the call is answered from the static type with no observable
//     boxing". It removes the BOX, not the receiver.
//
// ── WHAT MAKES EACH CASE ABLE TO FAIL ────────────────────────────────────────
//
// The pre-fix program EXITS 0 and prints a CORRECT `"[3]int"`, so neither "it
// ran" nor "the rendering is right" is an oracle here — both hold on the broken
// compiler. Every case below therefore asserts the EXACT stdout including the
// receiver's own marker line, and the multi-receiver cases interleave numbered
// markers so a fix that lowered the receiver the wrong NUMBER of times, or in
// the wrong ORDER, fails as loudly as one that did not lower it at all. T7 adds
// an explicit COUNTER, read back as text, because "evaluated twice" is the
// specific regression this fix shape risks and no ordinary value assertion
// would see it.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// `Make` returns a `[3]int` and also performs observable I/O, so `C.Make()` is
// an inline-array-valued expression with a side effect. `Say` is the same thing
// at `[2]int` with the marker chosen by the caller, for the ordering cases.
// `Ctr` is a plain mutable-field class used as an evaluation COUNTER: `Bump`
// increments it and returns a `[2]int`, so the number of times a receiver
// expression ran is readable back as text.
constexpr const char* kDecls =
    "module M3445; "
    "class Ctr "
    "  public N: int := 0; "
    "end Ctr; "
    "class R "
    "  public V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end R; "
    "class Holder "
    "  public Arr: [3]int; "
    "  public constructor() Arr := [7, 8, 9]; end constructor; "
    "end Holder; "
    "class C "
    "  public static method Make(): [3]int "
    "    Console.WriteLine(\"side-arr\"); "
    "    var a: [3]int := [1, 2, 3]; "
    "    return a; "
    "  end Make; "
    "  public static method Say(s: string): [2]int "
    "    Console.WriteLine(s); "
    "    var a: [2]int := [1, 2]; "
    "    return a; "
    "  end Say; "
    "  public static method Bump(c: Ctr): [2]int "
    "    c.N := c.N + 1; "
    "    var a: [2]int := [1, 2]; "
    "    return a; "
    "  end Bump; ";

std::string prog(const std::string& body) {
    return std::string(kDecls) +
           "  public static method Main(): int " + body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ── T1 — THE ISSUE'S EXACT REPRO. Pre-fix: "A\n[3]int\nB\n" (exit 0, no
// diagnostic at any severity). Post-fix the receiver's own `Console.WriteLine`
// appears between them and the §8.5 rendering is unchanged — the fix must ADD
// the effect, not trade the rendering for it.
BEZEL_TEST(codegen_inline_array_receiver_expression_evaluated_3445,
           inline_array_valued_receiver_call_is_evaluated_spec_22_3_8_5) {
    auto r = runAot(prog(
        "    Console.WriteLine(\"A\"); "
        "    Console.WriteLine(C.Make().ToString()); "
        "    Console.WriteLine(\"B\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("A\nside-arr\n[3]int\nB\n"));
}

// ── T2 — EXACTLY ONCE, IN ORDER (§22.3's "evaluated exactly once", §22.5's
// "call-site textual order"). Three inline-array-valued receivers with distinct
// markers, interleaved with numbered statements. A receiver lowered twice,
// lowered out of order, or hoisted out of the statement it belongs to all
// produce a different string here; only "once, in place" produces this one.
BEZEL_TEST(codegen_inline_array_receiver_expression_evaluated_3445,
           inline_array_receiver_evaluated_once_in_order_spec_22_3_22_5) {
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
                    std::string("r1\n1[2]int\nr2\n2[2]int\nr3\n3[2]int\n"));
}

// ── T3 — the EXPRESSION-STATEMENT form, where the `"[2]int"` result is
// discarded and the receiver's effect is the only thing the statement can be
// for. Pre-fix this emitted NOTHING AT ALL between the markers — the entire
// statement was a no-op with no diagnostic at any severity.
BEZEL_TEST(codegen_inline_array_receiver_expression_evaluated_3445,
           inline_array_receiver_in_discarded_expression_statement_spec_22_3) {
    auto r = runAot(prog(
        "    Console.WriteLine(\"A\"); "
        "    C.Say(\"D\").ToString(); "
        "    Console.WriteLine(\"B\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("A\nD\nB\n"));
}

// ── T4 — the ARGUMENT position, with TWO inline-array-valued receivers in one
// call, so §22.5's "call-site textual order … is normative" is pinned and not
// merely the count. Pre-fix stdout was "[2]int[2]int\n" alone; both receiver
// markers were gone.
BEZEL_TEST(codegen_inline_array_receiver_expression_evaluated_3445,
           two_inline_array_receivers_in_one_call_in_order_spec_22_5) {
    auto r = runAot(prog(
        "    Console.WriteLine(C.Say(\"L\").ToString() + "
        "                      C.Say(\"R\").ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("L\nR\n[2]int[2]int\n"));
}

// ── T5 — the INTERPOLATION-HOLE form (§18.5). A different consumer of the same
// lowering, reached through a different path in codegen, and broken identically
// pre-fix: `[[2]int]` with no "I".
BEZEL_TEST(codegen_inline_array_receiver_expression_evaluated_3445,
           inline_array_receiver_inside_interpolated_string_spec_18_5_22_3) {
    auto r = runAot(prog(
        "    Console.WriteLine($\"[{C.Say(\"I\").ToString()}]\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("I\n[[2]int]\n"));
}

// ── T6 — the result is REALLY §8.5's six-character `"[3]int"`, not merely
// something that prints like it. Chaining `.Length` off the answer would read a
// null pointer if the fix had produced the effect at the cost of the value.
BEZEL_TEST(codegen_inline_array_receiver_expression_evaluated_3445,
           inline_array_receiver_tostring_result_is_the_spec_8_5_string) {
    auto r = runAot(prog(
        "    var n := C.Make().ToString().Length; "
        "    Console.WriteLine(n.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side-arr\n6\n"));
}

// ── T7 — the explicit COUNTING probe. `Bump` increments a field, so the number
// of evaluations is read back as text rather than inferred from line order.
// Pre-fix: "[2]int\nafter1=0\nTrue\nafter2=2\n" — the `ToString` receiver ran
// ZERO times while `Equals`, which needs the receiver's bytes and therefore
// lowered it, already ran BOTH its receiver and its argument. Post-fix
// `after1=1` (exactly once, per §22.3) and `after2=3` (the untouched `Equals`
// arm still runs exactly its own two, so this row also proves the fix did not
// perturb the sibling members that were already correct). `after1=2` — the
// specific regression this fix shape risks — fails here and nowhere else.
BEZEL_TEST(codegen_inline_array_receiver_expression_evaluated_3445,
           inline_array_receiver_evaluation_count_is_one_spec_22_3) {
    auto r = runAot(prog(
        "    using var c := new Ctr(); "
        "    Console.WriteLine(C.Bump(c).ToString()); "
        "    Console.WriteLine(\"after1=\" + c.N.ToString()); "
        "    Console.WriteLine(C.Bump(c).Equals(C.Bump(c)).ToString()); "
        "    Console.WriteLine(\"after2=\" + c.N.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("[2]int\nafter1=1\nTrue\nafter2=3\n"));
}

// ── T8 — CONTROL: the LVALUE receiver shapes that ALREADY worked and must be
// byte-identical after the fix. A local, a field, and an element read taken
// AFTER the `ToString` — the fix newly emits a value-load of the whole array on
// these paths, so a load that disturbed the storage (or a receiver lowered
// twice through an `lvalueAddr`-then-`lowerExpr` fallback) shows up here as a
// wrong element or a duplicated line.
BEZEL_TEST(codegen_inline_array_receiver_expression_evaluated_3445,
           lvalue_inline_array_receivers_unchanged_spec_15_1_8_5) {
    auto r = runAot(prog(
        "    var loc: [3]int := [4, 5, 6]; "
        "    Console.WriteLine(loc.ToString()); "
        "    Console.WriteLine(loc[0].ToString()); "
        "    Console.WriteLine(loc[2].ToString()); "
        "    using var h := new Holder(); "
        "    Console.WriteLine(h.Arr.ToString()); "
        "    Console.WriteLine(h.Arr[1].ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("[3]int\n4\n6\n[3]int\n8\n"));
}

// ── T9 — CONTROL on the MOVE-ONLY form. §8.5 covers "inline arrays (all forms,
// including move-only `[N owned]…` arrays, whose receiver is borrowed for the
// call)", and §15.1/§15.3 make `owned` part of the type, so the rendering is
// `"[2 owned]R"` and not `"[2]R"` (#2959's single-spelling authority). This is
// the row where the fix's new value-load is most dangerous: the elements are
// owned pointers, and a load that were treated as a move would leave the local
// unusable or double-dispose it. The reads AFTER the `ToString` are the oracle;
// the row is also run under the gate's ASan/LSan oracle, where a disposal fault
// would surface directly.
BEZEL_TEST(codegen_inline_array_receiver_expression_evaluated_3445,
           owned_inline_array_local_receiver_unchanged_spec_8_5_15_3) {
    auto r = runAot(prog(
        "    var oa: [2 owned]R := [new R(1), new R(2)]; "
        "    Console.WriteLine(oa.ToString()); "
        "    Console.WriteLine(oa[0].V.ToString()); "
        "    Console.WriteLine(oa[1].V.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("[2 owned]R\n1\n2\n"));
}
