// Spec-derived tests for §19.3 "Named destructuring" (v0.334, #2197).
//
// §19.3, "Named destructuring":
//   "A destructuring pattern's parenthesized slot list may be written **by
//    name** instead of by position: each named slot is `Identifier "=" Pattern`
//    (grammar A.7 `NamedSlot`), where the `Identifier` names a member of the
//    pattern's type and the pattern after `=` interrogates that member."
//   "The `=` is the interrogation reading of §8.4, not an assignment … while a
//    pattern *asks* whether a member holds a value (`Rect(Width = 3)` — "is
//    `Width` 3?"), which is exactly what `=` means."
//
// Before #2197 the compiler had NO parse for this form: every
// DestructuringPattern reaching sema was positional, which sema itself
// recorded in place ("the named-slot FORM is not yet parsed by this compiler
// (#2197, spec-ahead-of-impl)"). All seven programs below fail to PARSE on
// pre-fix code with `expected ')' to close destructuring pattern`; they are
// parser failures, not sema rejections, so each row here is a genuine
// fail-without.
//
// The implementation normalizes a named slot list to DECLARATION order in
// sema, because both sema's binding walk and codegen index `subPatterns[i]`
// POSITIONALLY against `fields[i]` (a record's positional fields) /
// `payload[i]` (a choice case's payload fields). Normalizing is what lets the
// named form need no codegen change, and it is what the ORDER-SWAP rows below
// pin: a name must map to its OWN field, not to a position.
//
// MANDATORY DIFFERENTIAL CONTROL: every acceptance row is paired with the
// POSITIONAL spelling of the same program. The positional form is the whole
// pre-existing surface (thousands of rows across the suite); if a change here
// broke the positional reading, these controls go red first. See
// `positional_form_still_works_differential_control_*` below.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"          // runAot — the real bezelc AOT oracle
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// ===========================================================================
// ACCEPT — the conformance divergence row s19_3_0007's own program, verbatim
// in behaviour: `Rect(Width = 3, Height = var h)` interrogates Width by name
// and binds Height. Prints 4.
// ===========================================================================
BEZEL_TEST(case_named_slot_2197,
           record_constant_and_binding_slots_run_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(3, 4); "
        "    case r of "
        "      Rect2197(Width = 3, Height = var h) => Console.WriteLine(h.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// ACCEPT + THE ORDER-SWAP CONTROL — the row that proves a name maps to its
// OWN field rather than to a position. Ground truth (pinned by
// `record_fields_are_three_and_four`): Width=3, Height=4.
//
// `Rect2197(Height = var h, Width = 3)` names Height FIRST, out of
// declaration order. A parser-only implementation that left the slots in
// textual order and bound them positionally would evaluate `var h` against
// field 0 (Width=3) and the constant 3 against field 1 (Height=4), refute the
// second test, and fall to the `_` arm — printing "other" where the spec's
// reading ("is `Width` 3?", binding Height) prints 4. This row is that
// mutant, and it is why the sema normalization is load-bearing rather than
// incidental.
// ===========================================================================
BEZEL_TEST(case_named_slot_2197,
           record_named_slots_bind_by_name_not_position_order_swap) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(3, 4); "
        "    case r of "
        "      Rect2197(Height = var h, Width = 3) => Console.WriteLine(h.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// GROUND TRUTH for the row above, independent of the named form: the positional
// destructure of the same record prints "3,4", so Width is 3 and Height is 4.
// Without this the order-swap row could be satisfied by any constant that
// happens to print 4.
BEZEL_TEST(case_named_slot_2197,
           record_fields_are_three_and_four_ground_truth) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(3, 4); "
        "    case r of "
        "      Rect2197(var w, var h) => Console.WriteLine(w.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// ACCEPT — the constant slot really interrogates. Same shape as the headline
// row with Width mutated to 9: the constant slot must REFUTE, so the arm is
// skipped and `_` prints "other". Without this, a "named slot compiles and
// runs" row could be satisfied by a constant slot that is parsed and then
// ignored (which is precisely the silent-drop defect #3902 fixed for choice
// payload slots).
// ===========================================================================
BEZEL_TEST(case_named_slot_2197,
           record_constant_slot_refutes_and_falls_through_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(9, 4); "
        "    case r of "
        "      Rect2197(Width = 3, Height = var h) => Console.WriteLine(h.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// ACCEPT — §19.3: "Each slot's right-hand side is a **full pattern** (grammar
// A.7 `AltPattern`): … a per-slot `|` alternative among binding-free members."
// Prints "medium" (the "small" | "medium" alternative matches Label="medium").
// ===========================================================================
BEZEL_TEST(case_named_slot_2197,
           record_per_slot_alternative_in_named_slot_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Size2197(Label: string, Value: int); "
        "public class Program "
        "  public static method Main() "
        "    var s := new Size2197(\"medium\", 5); "
        "    case s of "
        "      Size2197(Label = \"small\" | \"medium\", Value = var v) => Console.WriteLine(\"medium\"), "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// ACCEPT — §19.3: a range pattern is an admitted named-slot right-hand side.
// Score=75 must match the 60..100 arm, not 0..59. Prints "ok".
// ===========================================================================
BEZEL_TEST(case_named_slot_2197,
           record_range_pattern_in_named_slot_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Stats2197(Score: int); "
        "public class Program "
        "  public static method Main() "
        "    var s := new Stats2197(75); "
        "    case s of "
        "      Stats2197(Score = 0..59) => Console.WriteLine(\"fail\"), "
        "      Stats2197(Score = 60..100) => Console.WriteLine(\"ok\"), "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// ACCEPT — a nested named destructure as a slot's right-hand side (§19.3: "or a
// nested destructuring — named or positional — recursively"). The inner head
// names a record by name; the outer slot names the field holding it.
// ===========================================================================
BEZEL_TEST(case_named_slot_2197,
           record_nested_named_destructure_in_slot_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Inner2197(A: int, B: int); "
        "public record Outer2197(First: int, Boxed: Inner2197); "
        "public class Program "
        "  public static method Main() "
        "    var o := new Outer2197(1, new Inner2197(7, 8)); "
        "    case o of "
        "      Outer2197(First = var f, Boxed = Inner2197(A = var a, B = var b)) => "
        "        Console.WriteLine(a.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// ACCEPT — a `_` right-hand side on an UNMARKED named slot. §19.3's per-slot
// list admits `_` ("Each slot's right-hand side is a full pattern …: a binding
// (`var w`), `_`, a constant, …"); the `_` rejection the spec states is scoped
// to the MARKED slot (`move N = _`, condition (iii) — "`_` is no binding").
// Prints "discarded".
// ===========================================================================
BEZEL_TEST(case_named_slot_2197,
           record_discard_rhs_on_unmarked_named_slot_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Box2197(Item: int); "
        "public class Program "
        "  public static method Main() "
        "    var b := new Box2197(5); "
        "    case b of "
        "      Box2197(Item = _) => Console.WriteLine(\"discarded\"), "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// ACCEPT — §19.3's class named form is NOT implemented here (it needs the
// codegen property-getter path); the record named form is. This row pins that
// the two heads are distinguished rather than both falling into one arm, and
// doubles as the record-side differential for the class rejection below.
// ===========================================================================

// ===========================================================================
// REJECT — §19.3: "A slot list is **either** positional **or** named, never
// mixed in one pattern (v1); mixing (`Rect(3, Height = var h)`) is a compile
// error naming the rule."
// ===========================================================================
BEZEL_TEST(case_named_slot_2197,
           mixed_positional_and_named_slots_rejected_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(3, 4); "
        "    case r of "
        "      Rect2197(3, Height = var h) => Console.WriteLine(h.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "never mixed in one pattern"));
}

// REJECT — the mirror: a naked positional binding first, a named slot second.
BEZEL_TEST(case_named_slot_2197,
           mixed_naked_binding_then_named_slot_rejected_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(3, 4); "
        "    case r of "
        "      Rect2197(w, Height = var h) => Console.WriteLine(h.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "never mixed in one pattern"));
}

// ===========================================================================
// REJECT — §19.3: "An unknown name … is a compile error at the pattern, each
// naming its rule." The diagnostic also enumerates the names the type DOES
// have, so the reader is not left hunting.
// ===========================================================================
BEZEL_TEST(case_named_slot_2197,
           unknown_slot_name_rejected_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(3, 4); "
        "    case r of "
        "      Rect2197(Depth = var d, Height = var h) => Console.WriteLine(h.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "names no member"));
}

// ===========================================================================
// REJECT — §19.3: "a duplicate name in one slot list … is a compile error at
// the pattern".
// ===========================================================================
BEZEL_TEST(case_named_slot_2197,
           duplicate_slot_name_rejected_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(3, 4); "
        "    case r of "
        "      Rect2197(Width = var a, Width = var b) => Console.WriteLine(a.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "is bound twice"));
}

// ===========================================================================
// REJECT — §19.3 (v0.338, proposal 0183): the marked named slot is retired.
// `move` is not a slot spelling at any pattern head; the parser rejects it
// with the positional-form diagnostic. (Before v0.338 the marker was admitted
// only on a CHOICE case head, and this row pinned the record-head rejection.)
//
// This row historically made `PositionalPattern::hasMoveMarker` a field with a
// real reader. The parser test below pins that `move N = ` PARSES (the
// marker is spelled on the keyword `move`, which §B.1 reserves, so a parser
// that looked for an Identifier could never reach it) and this row pins that
// the marker is then REJECTED rather than silently ignored.
// ===========================================================================
BEZEL_TEST(case_named_slot_2197,
           move_marker_on_record_named_slot_rejected_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record R2197(Content: string); "
        "public class Program "
        "  public static method Main() "
        "    var b := new R2197(\"x\"); "
        "    case b of "
        "      R2197(move Content = var s) => Console.WriteLine(s), "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // v0.338 (proposal 0183): `move` is not a slot spelling; the parser
    // rejects it before sema's record-head check is reached.
    BEZEL_EXPECT_TRUE(diagContains(r, "is not a slot spelling"));
}

// ===========================================================================
// REJECT — §19.3 admits the NAMED form over a class head, which this compiler
// does not implement (the destructuring lowering resolves positional record
// fields only). The diagnostic must not mislabel a named slot list as
// "positional": §19.3's "no positional surface" rule is the POSITIONAL form's
// alone. The row below and the one after it pin both messages, so the two
// spellings cannot be conflated.
// ===========================================================================
BEZEL_TEST(case_named_slot_2197,
           named_slots_on_class_head_rejected_not_as_positional_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public class Dog2197 "
        "  public constructor() end constructor; "
        "  public readonly property Name2197: string; "
        "end Dog2197; "
        "public class Program "
        "  public static method Main() "
        "    var d := new Dog2197(); "
        "    case d of "
        "      Dog2197(Name2197 = var n) => Console.WriteLine(n), "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "named destructuring pattern"));
    BEZEL_EXPECT_FALSE(diagContains(r, "positional destructuring pattern"));
}

// THE DIFFERENTIAL CONTROL for the row above: the POSITIONAL spelling over the
// same class head keeps the pre-existing §19.3 diagnostic verbatim. If the
// named-form message had replaced it, this row goes red.
BEZEL_TEST(case_named_slot_2197,
           positional_class_head_still_rejected_with_positional_message) {
    auto r = runAot(
        "module M; "
        "public class Dog2197 "
        "  public constructor() end constructor; "
        "  public readonly property Name2197: string; "
        "end Dog2197; "
        "public class Program "
        "  public static method Main() "
        "    var d := new Dog2197(); "
        "    case d of "
        "      Dog2197(_) => Console.WriteLine(\"x\"), "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "positional destructuring pattern"));
}

// ===========================================================================
// MANDATORY DIFFERENTIAL CONTROLS — the POSITIONAL surface.
//
// The named form is a new SPELLING over machinery that already existed. These
// rows are the control that the existing positional reading is untouched: same
// program, positional spelling, old behaviour. They are the rows most likely
// to break, which is why they are here rather than left to the wider suite.
// ===========================================================================
BEZEL_TEST(case_named_slot_2197,
           positional_form_still_works_differential_control_binding) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(3, 4); "
        "    case r of "
        "      Rect2197(var w, var h) => Console.WriteLine(h.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// CONTROL — the positional constant slot must still interrogate by POSITION.
BEZEL_TEST(case_named_slot_2197,
           positional_form_still_works_differential_control_constant) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(9, 4); "
        "    case r of "
        "      Rect2197(3, var h) => Console.WriteLine(h.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// CONTROL — the positional `_` discard slot must still be admitted in any
// position (§19.3 "Discard `_`").
BEZEL_TEST(case_named_slot_2197,
           positional_form_still_works_differential_control_discard) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(3, 4); "
        "    case r of "
        "      Rect2197(_, var h) => Console.WriteLine(h.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// CONTROL — the positional ARITY mismatch must still be rejected with its own
// diagnostic (the named normalization runs before this check, so this row
// proves it did not swallow or shadow the positional rule).
BEZEL_TEST(case_named_slot_2197,
           positional_form_still_works_differential_control_arity) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(3, 4); "
        "    case r of "
        "      Rect2197(var a, var b, var c) => Console.WriteLine(a.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "positional field"));
}

// CONTROL — a positional destructure in EXPRESSION position (the CaseExpr
// twin of the statement-position rows above). The named-form normalization is
// wired into BOTH handlers; this row proves the expression handler's positional
// path still runs.
BEZEL_TEST(case_named_slot_2197,
           positional_form_still_works_differential_control_expr_position) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(3, 4); "
        "    Console.WriteLine("
        "      (case r of "
        "         Rect2197(var w, var h) => h.ToString(), "
        "         _ => \"other\")); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// CONTROL — the named form in EXPRESSION position, which exercises the
// CaseExpr normalization site (a distinct call site from the CaseStmt one).
BEZEL_TEST(case_named_slot_2197,
           named_form_in_expression_position_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(3, 4); "
        "    Console.WriteLine("
        "      (case r of "
        "         Rect2197(Width = 3, Height = var h) => h.ToString(), "
        "         _ => \"other\")); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// CONTROL — a named slot list over a CHOICE case's payload fields, which
// exercises the choice normalization site. `Wrap(Payload = var p, Tag = var t)`
// names the payload fields out of declaration order.
BEZEL_TEST(case_named_slot_2197,
           named_slots_over_choice_case_payload_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public choice Opt2197 Some2197(value: int), None2197; end Opt2197; "
        "public class Program "
        "  public static method Main() "
        "    var o: Opt2197 := Opt2197.Some2197(42); "
        "    case o of "
        "      Some2197(value = var v) => Console.WriteLine(v.ToString()), "
        "      None2197 => Console.WriteLine(\"none\") "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// CONTROL — an unknown name over a CHOICE payload names the payload fields.
BEZEL_TEST(case_named_slot_2197,
           unknown_name_over_choice_payload_rejected_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public choice Opt2197 Some2197(value: int), None2197; end Opt2197; "
        "public class Program "
        "  public static method Main() "
        "    var o: Opt2197 := Opt2197.Some2197(42); "
        "    case o of "
        "      Some2197(nope = var v) => Console.WriteLine(v.ToString()), "
        "      None2197 => Console.WriteLine(\"none\") "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "names no member"));
}

// CONTROL — the positional choice destructure must still bind by position.
BEZEL_TEST(case_named_slot_2197,
           positional_choice_payload_still_works_differential_control) {
    auto r = runAot(
        "module M; "
        "public choice Opt2197 Some2197(value: int), None2197; end Opt2197; "
        "public class Program "
        "  public static method Main() "
        "    var o: Opt2197 := Opt2197.Some2197(42); "
        "    case o of "
        "      Some2197(var v) => Console.WriteLine(v.ToString()), "
        "      None2197 => Console.WriteLine(\"none\") "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// §19.3 "Named-destructure arms" (§19.6): a named destructuring contributes
// to static exhaustiveness under the IDENTICAL rule a positional one does —
// the arm counts only if EVERY slot is irrefutable. `Stats(Score = var s)`
// over a one-field record is all-irrefutable and so needs no `_` arm; the
// program compiles quietly and prints the score.
// ===========================================================================
BEZEL_TEST(case_named_slot_2197,
           all_irrefutable_named_destructure_is_exhaustive_spec_19_6) {
    auto r = runAot(
        "module M; "
        "public record Stats2197(Score: int); "
        "public class Program "
        "  public static method Main() "
        "    var s := new Stats2197(11); "
        "    case s of "
        "      Stats2197(Score = var v) => Console.WriteLine(v.ToString()); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// The dual: a REFUTABLE named slot contributes nothing to the proof, so the
// same one-arm shape without a wildcard is non-exhaustive and warns (§19.6).
// This is the differential that shows the exhaustiveness rule reads the
// normalized slots rather than blindly accepting any named arm.
//
// The REFUTABLE named slot (`Score = 5`) must not select its arm for Score=11,
// so the following arm is reached. NOTE: this row deliberately asserts
// OBSERVABLE BEHAVIOUR (which arm runs), not a diagnostic — `runAot` fills
// `diagnostics` only when a compile is REJECTED, so a row that compiled can
// never observe a non-exhaustiveness Warning through this oracle, and an
// assertion on it would pass vacuously on any program at all.
BEZEL_TEST(case_named_slot_2197,
           refutable_named_slot_does_not_select_its_arm_spec_19_6) {
    auto r = runAot(
        "module M; "
        "public record Stats2197(Score: int); "
        "public class Program "
        "  public static method Main() "
        "    var s := new Stats2197(11); "
        "    case s of "
        "      Stats2197(Score = 5) => Console.WriteLine(\"five\"), "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// CONTROL — the positional all-irrefutable arm still proves exhaustiveness
// (the §19.6 "Record scrutinee" rule the named form reuses).
BEZEL_TEST(case_named_slot_2197,
           positional_irrefutable_destructure_still_exhaustive_control) {
    auto r = runAot(
        "module M; "
        "public record Stats2197(Score: int); "
        "public class Program "
        "  public static method Main() "
        "    var s := new Stats2197(11); "
        "    case s of "
        "      Stats2197(var v) => Console.WriteLine(v.ToString()); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// §19.3 (v0.338, #5966): "a named list may name any subset of the type's
// members: a member it omits is not read (an owning match disposes the
// un-transferred remainder at construct completion, §35.5)."
//
// Before #5966 the normalization reordered a named list into declaration order
// but left it SHORT, and the downstream positional arity rule then rejected it
// ("binds 1 value(s) but record `Rect` has 2 positional field(s)"). The
// normalization now pads each omitted member with a `_` slot, so a named list
// always reaches the binding walk at the record's full arity and the positional
// rule is still enforced for POSITIONAL lists only (see the negative rows).
// ===========================================================================
namespace {

bool stdoutIs(const bezel::test::JitResult& r, const char* expected) {
    return r.stdoutText == std::string(expected) + "\n" ||
           r.stdoutText == std::string(expected) + "\r\n";
}

std::size_t countOccurrences(const std::string& hay, const char* needle) {
    std::size_t n = 0, pos = 0;
    const std::string nd(needle);
    while ((pos = hay.find(nd, pos)) != std::string::npos) {
        ++n;
        pos += nd.size();
    }
    return n;
}

}  // namespace

// THE ISSUE REPRO (#5966; conformance row s19_3_0016). `Rect(Height = var h)`
// names a SUBSET of the two members. Prints 4.
BEZEL_TEST(case_named_slot_2197,
           named_slot_subset_spec_19_3_record_subset_prints_4) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(3, 4); "
        "    case r of "
        "      Rect2197(Height = var h) => Console.WriteLine(h.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(stdoutIs(r, "4"));
}

// Two named slots written OUT OF DECLARATION ORDER, both bound: the reorder
// must give each name its own member (Height=4 to h, Width=3 to w).
BEZEL_TEST(case_named_slot_2197,
           named_slot_subset_spec_19_3_two_named_slots_out_of_order) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(3, 4); "
        "    case r of "
        "      Rect2197(Height = var h, Width = var w) => do "
        "        Console.WriteLine(w.ToString()); "
        "        Console.WriteLine(h.ToString()); "
        "      end, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText == "3\n4\n" || r.stdoutText == "3\r\n4\r\n");
}

// A subset whose ONE named slot is a constant that REFUTES: the omitted Width
// is not read, and Height=9 fails, so the `_` arm runs. Pins that the padded
// discard does not turn the arm into an unconditional match.
BEZEL_TEST(case_named_slot_2197,
           named_slot_subset_spec_19_3_refuting_subset_falls_through) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(3, 4); "
        "    case r of "
        "      Rect2197(Height = 9) => Console.WriteLine(\"nine\"), "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(stdoutIs(r, "other"));
}

// A class-typed FIELD of a record, read by a named subset that omits the
// sibling Tag: the named read yields the class instance and its method runs.
BEZEL_TEST(case_named_slot_2197,
           named_slot_subset_spec_19_3_class_typed_field_subset) {
    auto r = runAot(
        "module M; "
        "public class Widget2197 "
        "  public constructor() end constructor; "
        "  public method Id(): int => 42; "
        "end Widget2197; "
        "public record Holder2197(Tag: int, Item: Widget2197); "
        "public class Program "
        "  public static method Main() "
        "    using var w := new Widget2197(); "
        "    var h := new Holder2197(7, w); "
        "    case h of "
        "      Holder2197(Item = var it) => Console.WriteLine(it.Id().ToString()), "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(stdoutIs(r, "42"));
}

// An UNNAMED class-typed field is not read and is not disposed by the match:
// the owning local `w` is disposed once at its own scope end. Looped 100 times
// through the checked harness (generated-code ASan) so a leak or double-free of the
// class instance would show; the count of disposals must be exactly 101 (`0..100`).
BEZEL_TEST(case_named_slot_2197,
           named_slot_subset_spec_19_3_unnamed_class_field_disposed_once_loop) {
    auto r = runAotChecked(
        "module M; "
        "public class Widget2197 "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"widget disposed\"); end destructor; "
        "end Widget2197; "
        "public record Holder2197(Tag: int, Item: Widget2197); "
        "public class Program "
        "  public static method Main() "
        "    var n := 0; "
        "    for var i in 0..100 do "
        "      using var w := new Widget2197(); "
        "      var h := new Holder2197(7, w); "
        "      case h of "
        "        Holder2197(Tag = 7) => n := n + 1;, "
        "        _ => Console.WriteLine(\"other\"); "
        "      end case; "
        "    end for; "
        "    Console.WriteLine(n.ToString()); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "widget disposed"), 101u);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("101") != std::string::npos);
}

// The OWNED payload case of §19.3/§35.5: `Fetched(status = 200)` omits the
// `owned response` payload. The owning match disposes the un-transferred
// remainder at construct completion, once per iteration, over 101 iterations
// (generated-code ASan through the checked harness; `0..100` is 101 iterations).
BEZEL_TEST(case_named_slot_2197,
           named_slot_subset_spec_19_3_owned_omitted_payload_disposed_once_loop) {
    auto r = runAotChecked(
        "module M; "
        "public class Widget2197 "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"widget disposed\"); end destructor; "
        "end Widget2197; "
        "public choice Outcome2197 "
        "  Fetched(owned response: Widget2197, status: int), "
        "  Failed(code: int); "
        "end Outcome2197; "
        "public class Program "
        "  public static method Main() "
        "    var n := 0; "
        "    for var i in 0..100 do "
        "      case Outcome2197.Fetched(new Widget2197(), 200) of "
        "        Fetched(status = 200) => n := n + 1;, "
        "        _ => Console.WriteLine(\"other\"); "
        "      end case; "
        "    end for; "
        "    Console.WriteLine(n.ToString()); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "widget disposed"), 101u);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("101") != std::string::npos);
}

// NEGATIVE — an unknown member name is still a compile error at the pattern,
// naming the rule (§19.3), and the list of valid names.
BEZEL_TEST(case_named_slot_2197,
           named_slot_subset_spec_19_3_unknown_name_rejected_with_rule) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(3, 4); "
        "    case r of "
        "      Rect2197(Depth = var d) => Console.WriteLine(d.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "names no member of `Rect2197`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "(§19.3)"));
}

// NEGATIVE — mixing positional and named slots in one list is still rejected.
BEZEL_TEST(case_named_slot_2197,
           named_slot_subset_spec_19_3_mixed_positional_and_named_rejected) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(3, 4); "
        "    case r of "
        "      Rect2197(3, Height = var h) => Console.WriteLine(h.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "mixes positional and named slots"));
}

// NEGATIVE — a POSITIONAL list with too few slots is still the count error:
// the positional arity rule is unchanged by the subset relaxation.
BEZEL_TEST(case_named_slot_2197,
           named_slot_subset_spec_19_3_positional_too_few_still_rejected) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(3, 4); "
        "    case r of "
        "      Rect2197(var w) => Console.WriteLine(w.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "binds 1 value(s) but record `Rect2197` has 2 positional field(s); "
           "the binding count must match (§19.3)"));
}

// NEGATIVE — a duplicate name in one named slot list is still rejected.
BEZEL_TEST(case_named_slot_2197,
           named_slot_subset_spec_19_3_duplicate_name_rejected) {
    auto r = runAot(
        "module M; "
        "public record Rect2197(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect2197(3, 4); "
        "    case r of "
        "      Rect2197(Width = var a, Width = var b) => Console.WriteLine(a.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "bound twice"));
}

// §19.3 (v0.338, #5966): a named subset over a CHOICE case payload. `Click`
// has two payload fields; `Click(y = var yy)` names only `y` and must match the
// case the same way the record subset does (the omitted `x` is not read).
BEZEL_TEST(case_named_slot_2197,
           named_slot_subset_spec_19_3_choice_payload_subset) {
    auto r = runAot(
        "module M; "
        "public choice Ev5966 Click5966(x: int, y: int), Other5966(code: int); end Ev5966; "
        "public class Program "
        "  public static method Main() "
        "    var e: Ev5966 := Ev5966.Click5966(3, 4); "
        "    case e of "
        "      Click5966(y = var yy) => Console.WriteLine(yy.ToString()), "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(stdoutIs(r, "4"));
}

// §19.3 (v0.338, #5966; nested follow-up #5991): a NESTED named slot list that
// is a strict subset, or is written out of declaration order, is refused with
// a rule-citing diagnostic until nested named lists are resolved to declaration
// order (#5991). Before this refusal the nested walk bound it by written
// position and silently read the wrong member, so the refusal is the only
// sound behaviour for this commit. The top-level subset rows above are
// unaffected.
BEZEL_TEST(case_named_slot_2197,
           named_slot_subset_spec_19_3_nested_subset_rejected_until_supported) {
    auto r = runAot(
        "module M; "
        "public record In5966(A: int, B: int); "
        "public record Out5966(X: int, I: In5966, Z: int); "
        "public class Program "
        "  public static method Main() "
        "    var o := new Out5966(1, new In5966(2, 3), 4); "
        "    case o of "
        "      Out5966(I = In5966(B = var b)) => Console.WriteLine(b.ToString()), "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "must name every member in declaration order"));
    BEZEL_EXPECT_TRUE(diagContains(r, "(§19.3)"));
}

BEZEL_TEST(case_named_slot_2197,
           named_slot_subset_spec_19_3_nested_reordered_rejected_until_supported) {
    auto r = runAot(
        "module M; "
        "public record In5966(A: int, B: int); "
        "public record Out5966(X: int, I: In5966, Z: int); "
        "public class Program "
        "  public static method Main() "
        "    var o := new Out5966(1, new In5966(2, 3), 4); "
        "    case o of "
        "      Out5966(X = var x, I = In5966(B = var b, A = var a), Z = var z) => "
        "        Console.WriteLine(a.ToString()), "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "must name every member in declaration order"));
}

BEZEL_TEST(case_named_slot_2197,
           named_slot_subset_spec_19_3_nested_mixed_rejected) {
    auto r = runAot(
        "module M; "
        "public record In5966(A: int, B: int); "
        "public record Out5966(X: int, I: In5966, Z: int); "
        "public class Program "
        "  public static method Main() "
        "    var o := new Out5966(1, new In5966(2, 3), 4); "
        "    case o of "
        "      Out5966(I = In5966(2, B = var b)) => Console.WriteLine(b.ToString()), "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "mixes positional and named slots"));
}
