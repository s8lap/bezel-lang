// An UNMARKED whole-value move of a case-arm pattern binding of MOVE-ONLY
// value type is a consume of a borrow — a heap-use-after-free (issue #4803;
// spec §15.3 *Pattern binding* / *Local-to-local assignment* / *Parameter
// passing*, §19.3, §35.5 *The borrowing match*, §28.3 K3.12, v0.307).
//
// THE RULE. §15.3 *Pattern binding*: "a `case` binding over a move-only
// scrutinee is a **borrow** of the value; consuming operations through the
// binding … are rejected". §35.5 *The borrowing match* repeats it for a
// move-only payload binding: "reads through the binding are admitted, and
// consuming operations through it … are rejected — the binding does not
// own." And a move-only value has no borrow reading at a store: §15.3's
// scope note, "copies are forbidden, so the plain binding can only move";
// *Parameter passing*, "for a move-only V, it is by value-move"; K3.12, "for
// move-only values, any store into an owning location" is a consume, spelled
// with no marker. So `var z := y;`, `z := y;`, `_ := y;`, `Take(y)` and an
// arm result `y` flowing into a store are each a MOVE of a borrow.
//
// THE BUG (#4803). The checker only rejected the MARKED spelling (`move y`,
// #4664); the unmarked one compiled. MEASURED pre-fix,
// `scripts/agent_check.sh --probe --asan`:
//
//   var o := Outer.W(new Widget("a"), new Widget("b"));
//   case o of
//     var y => do var z := y; Console.WriteLine("hit"); end
//   end case;
//
// printed `hit / dispose a / dispose b / after` and then faulted with
// heap-use-after-free in `main`: `z`'s scope-end pass disposed both payloads
// at arm exit, and `o`'s own scope-end active-case pass (§35.4) disposed them
// again. The same fault was measured for every reject row below except the
// argument and discard rows (not separately probed; same rule).
//
// OBSERVABILITY. The pre-fix behaviour is a CLEAN COMPILE, so the reject
// oracle is the SPECIFIC diagnostic text (absent from the pre-fix compiler);
// never a bare `!r.ran`, which a runtime crash also produces.
//
// SCOPE. The whole-value `var y` binding of an OWNING match's
// non-destructuring arm is deliberately NOT rejected here — see the control
// row and the open ruling #4821.
//
// per §15.3, §19.3, §35.5, §28.3 K3.12, v0.307

#include <string>

#include "support/aot_support.hpp"  // runAot / runAotChecked — strict AOT
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The exact diagnostic this fix introduces.
const char* kNeedle = "cannot move the `case` arm binding";

const char* kDecls =
    "public class Widget "
    "  private _n: string; "
    "  public constructor(n: string) _n := n; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_n}\"); end destructor; "
    "  public method Ping(): int => 7; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; "
    "public choice Box Wrap(inner: Outer), Nothing; end Box; ";

// Dispose-count variant for the loop-amplified accept rows.
const char* kCountingDecls =
    "public class Widget "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Widget._n := Widget._n + 1; end destructor; "
    "  public method Ping(): int => 7; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; "
    "public choice Box Wrap(inner: Outer), Nothing; end Box; ";

std::string prog(const char* decls, const std::string& extra,
                 const std::string& body) {
    return std::string("module M; ") + decls +
           "public class Program " + extra +
           "  public static method Main(): int " + body +
           "  end Main; "
           "end Program;";
}

bool hasErrorContaining(const JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

bool rejectsWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    return hasErrorContaining(r, needle);
}

void expectRun(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// --- REJECT rows: an unmarked move out of a borrow ------------------------

// (1) THE ISSUE'S REPRO, verbatim.
BEZEL_TEST(borrowing_match_move_only_binding_consume_4803,
           spec_15_3_issue_repro_var_binding_of_borrowed_choice_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(kDecls, "",
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case o of "
        "      var y => do var z := y; Console.WriteLine(\"hit\"); end "
        "    end case; "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "), kNeedle));
}

// (2) Assignment into an existing owning-choice local (§15.3 bullet 1's
// `y := x;` spelling). Pre-fix: heap-use-after-free.
BEZEL_TEST(borrowing_match_move_only_binding_consume_4803,
           spec_15_3_assignment_of_borrowed_choice_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(kDecls, "",
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    var outer := Outer.X; "
        "    case o of "
        "      var y => do outer := y; end "
        "    end case; "
        "    return 0; "), kNeedle));
}

// (3) Plain by-value parameter: §15.3 *Parameter passing* — "for a
// move-only V, it is by value-move".
BEZEL_TEST(borrowing_match_move_only_binding_consume_4803,
           spec_15_3_argument_of_borrowed_choice_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(kDecls,
        "  public static method Take(x: Outer): int return 1; end Take; ",
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case o of "
        "      var y => do var n := Take(y); Console.WriteLine($\"{n}\"); end "
        "    end case; "
        "    return 0; "), kNeedle));
}

// (4) Expression-position arm result flowing into a local: the construct's
// value IS the selected arm's. Pre-fix: heap-use-after-free.
BEZEL_TEST(borrowing_match_move_only_binding_consume_4803,
           spec_15_3_expression_arm_result_of_borrowed_choice_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(kDecls, "",
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    var r := (case o of var y => y); "
        "    return 0; "), kNeedle));
}

// (5) The OTHER move-only kind: an owning inline array scrutinee (§19.3's
// own example shape). Pre-fix: heap-use-after-free.
BEZEL_TEST(borrowing_match_move_only_binding_consume_4803,
           spec_19_3_var_binding_of_borrowed_owning_array_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(kDecls, "",
        "    var arr: [2 owned]Widget? := [new Widget(\"a\"), new Widget(\"b\")]; "
        "    case arr of "
        "      var s => do var t := s; end "
        "    end case; "
        "    return 0; "), kNeedle));
}

// (6) A move-only PAYLOAD binding of a borrowing match — §35.5's "a binding
// of a payload of move-only value type … is likewise a borrow".
// Pre-fix: heap-use-after-free.
BEZEL_TEST(borrowing_match_move_only_binding_consume_4803,
           spec_35_5_var_binding_of_borrowed_move_only_payload_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(kDecls, "",
        "    var bx := Box.Wrap(Outer.W(new Widget(\"a\"), new Widget(\"b\"))); "
        "    case bx of "
        "      Wrap(var i) => do var z := i; end, "
        "      Nothing => () "
        "    end case; "
        "    return 0; "), kNeedle));
}

// (7) A borrowing match over a PARAMETER scrutinee. Pre-fix:
// heap-use-after-free.
BEZEL_TEST(borrowing_match_move_only_binding_consume_4803,
           spec_35_5_var_binding_over_parameter_scrutinee_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(kDecls,
        "  public static method Peek(p: Outer): int "
        "    case p of var y => do var z := y; end end case; "
        "    return 1; "
        "  end Peek; ",
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    Console.WriteLine($\"{Peek(o)}\"); "
        "    return 0; "), kNeedle));
}

// (8) The move-only discard `_ := y;` consumes (disposes) its operand
// (§17.13) — disposing a borrow.
BEZEL_TEST(borrowing_match_move_only_binding_consume_4803,
           spec_17_13_discard_of_borrowed_choice_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(kDecls, "",
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case o of "
        "      var y => do _ := y; end "
        "    end case; "
        "    return 0; "), kNeedle));
}

// --- ACCEPT rows: loop-amplified, dispose-count exact ----------------------

// (9) Reads through the borrow stay admitted: a nested borrowing `case`
// over `y`, a member call on a payload. 100 iterations x 2 payloads,
// each disposed exactly once (by `o`), ASan/LSan clean.
BEZEL_TEST(borrowing_match_move_only_binding_consume_4803,
           spec_35_5_reads_through_borrowed_binding_admitted_dispose_once) {
    auto r = runAotChecked(prog(kCountingDecls,
        "  public static method Once(): int "
        "    var o := Outer.W(new Widget(), new Widget()); "
        "    var n := 0; "
        "    case o of "
        "      var y => do "
        "        n := (case y of W(var a, _) => a.Ping(), X => 0); "
        "      end "
        "    end case; "
        "    return n; "
        "  end Once; ",
        "    var s := 0; "
        "    for var i in 0..<100 do s := s + Once(); end; "
        "    Console.WriteLine($\"s={s} disposed={Widget._n}\"); "
        "    return 0; "));
    expectRun(r, "s=700 disposed=200\n");
}

// (10) The owning match's POSITIONAL move-only payload slot genuinely
// transfers (§35.5: "transfers into its binding at arm selection exactly as
// an `owned` payload does"), so `var z := i;` is an ordinary move of an
// owned local and must stay admitted — exactly-once disposal.
BEZEL_TEST(borrowing_match_move_only_binding_consume_4803,
           spec_35_5_owning_match_positional_move_only_payload_move_admitted) {
    auto r = runAotChecked(prog(kCountingDecls,
        "  public static method Once(): int "
        "    var bx := Box.Wrap(Outer.W(new Widget(), new Widget())); "
        "    case move bx of "
        "      Wrap(var i) => do var z := i; end, "
        "      Nothing => () "
        "    end case; "
        "    return 1; "
        "  end Once; ",
        "    var s := 0; "
        "    for var i in 0..<100 do s := s + Once(); end; "
        "    Console.WriteLine($\"s={s} disposed={Widget._n}\"); "
        "    return 0; "));
    expectRun(r, "s=100 disposed=200\n");
}

// (11) CONTROL — scope boundary. The whole-value binding of an OWNING
// match's non-destructuring arm is not rejected by this fix (open ruling
// #4821; codegen's whole-value mark makes it dispose once).
BEZEL_TEST(borrowing_match_move_only_binding_consume_4803,
           control_owning_match_whole_value_arm_binding_not_rejected) {
    auto r = runAotChecked(prog(kCountingDecls,
        "  public static method Once(): int "
        "    var o := Outer.W(new Widget(), new Widget()); "
        "    case move o of "
        "      var y => do var z := y; end "
        "    end case; "
        "    return 1; "
        "  end Once; ",
        "    var s := 0; "
        "    for var i in 0..<100 do s := s + Once(); end; "
        "    Console.WriteLine($\"s={s} disposed={Widget._n}\"); "
        "    return 0; "));
    expectRun(r, "s=100 disposed=200\n");
}

// (12) CONTROL — a CLASS-typed borrowing binding: `var z := y;` is a borrow
// of a borrow (§28.3 K3.13: "an unmarked read of a class-typed binding never
// consumes"), untouched by this fix.
BEZEL_TEST(borrowing_match_move_only_binding_consume_4803,
           control_class_typed_binding_plain_var_is_a_borrow) {
    auto r = runAotChecked(prog(kCountingDecls,
        "  public static method Once(): int "
        "    using var w := new Widget(); "
        "    var n := 0; "
        "    case w of "
        "      var y => do var z := y; n := z.Ping(); end "
        "    end case; "
        "    return n; "
        "  end Once; ",
        "    var s := 0; "
        "    for var i in 0..<100 do s := s + Once(); end; "
        "    Console.WriteLine($\"s={s} disposed={Widget._n}\"); "
        "    return 0; "));
    expectRun(r, "s=700 disposed=100\n");
}

// --- fix #4837: four previously-uncovered consumption paths -------------------

// (13) Interpolation operand: $"result={Take(y)}" moves y through the
// Take(y) call inside the interpolation. Pre-fix: compiled cleanly.
BEZEL_TEST(borrowing_match_move_only_binding_consume_4803,
           spec_15_3_interpolation_operand_consumes_borrowed_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(kDecls,
        "  public static method Take(x: Outer): int return 1; end Take; ",
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case o of "
        "      var y => do Console.WriteLine($\"result={Take(y)}\"); end "
        "    end case; "
        "    return 0; "), kNeedle));
}

// (14) Lambda body: a consuming call inside the lambda body.
// Pre-fix: compiled cleanly.
BEZEL_TEST(borrowing_match_move_only_binding_consume_4803,
           spec_15_3_lambda_body_consumes_borrowed_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(kDecls,
        "  public static method Take(x: Outer): int return 1; end Take; ",
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case o of "
        "      var y => do var f := lambda(): int => Take(y); Console.WriteLine($\"done\"); end "
        "    end case; "
        "    return 0; "), kNeedle));
}

// (15) Collection literal element: a consuming call as element.
// The element Take(y) moves y. Pre-fix: compiled cleanly.
// (A direct [y] element does not type-check: no standard owning collection
// can hold a move-only choice type, so the type error fires first.)
BEZEL_TEST(borrowing_match_move_only_binding_consume_4803,
           spec_15_3_collection_literal_element_consumes_borrowed_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(kDecls,
        "  public static method Take(x: Outer): int return 1; end Take; ",
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case o of "
        "      var y => do var arr: [1]int := [Take(y)]; Console.WriteLine($\"done\"); end "
        "    end case; "
        "    return 0; "), kNeedle));
}

// (16) yield return: a borrow-only case arm binding moved into the iterator's
// yield position. Pre-fix: compiled cleanly.
BEZEL_TEST(borrowing_match_move_only_binding_consume_4803,
           spec_17_14_yield_return_consumes_borrowed_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(kDecls,
        "  public static method Iter(): IEnumerable<Outer> "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case o of "
        "      var y => yield return y; "
        "    end case; "
        "  end Iter; ",
        "    var count := 0; "
        "    for var x in Iter() do count := count + 1; end; "
        "    return count; "), kNeedle));
}
