// fix #3443 — `o.Equals(<value-typed argument>)` on an `Object` / interface /
// class receiver skipped §8.6's boxing OF THE ARGUMENT, so the call lowered to
// nothing and the whole statement was dropped
// (spec §8.1 *Definition*, §8.3 *`Equals` and `GetHashCode`*, §8.6 *Boxing*,
// §5.1 *Reference conversions*, §7.5 *Null-conditional access (`?.`)* — v0.283).
//
// MEASURED on 2be0ed3 (this file's parent commit), with the issue's repro:
//
//     var o: Object := 7;  var q: Object := 7;  var i: int := 7;
//     Console.WriteLine(o.Equals(q).ToString());     // True   — worked
//     Console.WriteLine(o.Equals("x").ToString());   // False  — worked
//     Console.WriteLine(o.Equals(5).ToString());     // DROPPED
//     Console.WriteLine(o.Equals(i).ToString());     // DROPPED
//     Console.WriteLine(o.Equals(()).ToString());    // DROPPED
//
// The three dropped rows each drew
//
//     internal: the argument of this `Console.WriteLine` failed to lower to a
//     value, so the whole statement emits nothing — not even the line
//     terminator (stdlib §2.1 / §21.9) — #3079
//
// and printed NOTHING; the program still exited 0. Bound to a local instead of
// printed, the same rows drew the `#2434` note and the local was
// zero-initialized — i.e. `False`, a WRONG VALUE with only an `internal:` note,
// which is the silent shape. That an `Object`-typed argument (row A) and a
// `string` argument (row B) both worked is what localizes the defect: the
// dispatch is fine; only the ARGUMENT's boxing was missing.
//
// ── WHY IT IS A DEFECT ───────────────────────────────────────────────────────
//
//   * §8.1 declares the universal `public virtual method Equals(other: Object?):
//     bool`, so the argument position is an `Object` location.
//   * §5.1: "any type to `Object`: reference types … directly; value types
//     (primitives, records, enums, non-owning inline arrays, the unit type) by
//     boxing ([§8.6])".
//   * §8.6 states it outright: "When a value type is used where `Object` (or an
//     interface implemented by the value type) is expected, the value is
//     boxed: a heap-allocated wrapper holds the value."
//   * §8.6 *Nullable value-type boxing* fixes the two `int?` answers: "If the
//     value is `null`, the result is a null `Object` reference. If the value is
//     non-null, the result is a boxed `T` (the non-nullable form). The nullable
//     wrapper is stripped during boxing."
//
// Sema accepts every row (it types the result `bool`), so this was a codegen
// gap, not a missing rejection. It is fix #3422's sibling on the other operand:
// that one is the `()` RECEIVER of `Equals`/`GetHashCode`, this the value-typed
// ARGUMENT.
//
// ── ⚠ THE ISSUE'S EXPECTED VALUE FOR ROW D IS WRONG, DELIBERATELY NOT COPIED ──
//
// Issue 3443 predicts `False` for both `o.Equals(5)` and `o.Equals(i)` (i == 7,
// `o` boxing 7), reasoning from §8.4's fresh-box rule. That is right for `5` and
// wrong for `i`, and the issue contradicts itself two sentences later. §8.3's
// table gives the Primitive row "Value comparison" as the default `Equals`, so a
// boxed `int` compares BY VALUE, not by box identity — and the issue's own
// PASSING row A (`o.Equals(q)`, two independently boxed 7s, prints `True`) is
// the direct evidence. Row D is therefore `True`, and this file asserts `True`.
//
// ── WHAT MAKES EACH CASE ABLE TO FAIL ────────────────────────────────────────
//
// The pre-fix program EXITS 0, so an exit-code oracle is vacuous here — the
// broken rows print nothing at all while the surrounding rows print correctly.
// Every case asserts the EXACT stdout, and each row carries a letter so a
// MISSING line is distinguishable from a WRONG one.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

constexpr const char* kDecls =
    "module M3443; "
    "record Pt(X: int, Y: int); "
    "enum Color Red, Green; end Color; "
    "interface IShape method Area(): int; end IShape; "
    "class Sq : IShape "
    "  public S: int; "
    "  public constructor(s: int) S := s; end constructor; "
    "  public method Area(): int => 16; "
    "end Sq; "
    "class Plain "
    "  public V: int := 1; "
    "end Plain; "
    "class C "
    "  public static method Mark(v: int): int "
    "    Console.WriteLine(\"arg\" + v.ToString()); "
    "    return v; "
    "  end Mark; ";

std::string prog(const std::string& body) {
    return std::string(kDecls) +
           "  public static method Main(): int " + body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ── T1 — THE ISSUE'S EXACT REPRO. Pre-fix rows C, D and E printed NOTHING (three
// `#3079` notes, exit 0); A and B were already correct and are the localizing
// controls inside the same program.
BEZEL_TEST(codegen_object_equals_argument_boxing_3443,
           object_receiver_boxes_its_value_typed_argument_spec_8_6_5_1) {
    auto r = runAot(prog(
        "    var o: Object := 7; "
        "    var q: Object := 7; "
        "    var i: int := 7; "
        "    Console.WriteLine(\"A \" + o.Equals(q).ToString()); "
        "    Console.WriteLine(\"B \" + o.Equals(\"x\").ToString()); "
        "    Console.WriteLine(\"C \" + o.Equals(5).ToString()); "
        "    Console.WriteLine(\"D \" + o.Equals(i).ToString()); "
        "    Console.WriteLine(\"E \" + o.Equals(()).ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("A True\nB False\nC False\nD True\n"
                                "E False\n"));
}

// ── T2 — §5.1's value-kind LIST, one row per kind: a positional record
// (§8.3 field-by-field), an enum (underlying integer), `bool`, `double`, `char`,
// and a non-owning inline array (§8.3's element-by-element row). Each is
// compared against both an equal and an unequal value, so a fix that boxed but
// dispatched to a constant would fail on the second half of every pair.
BEZEL_TEST(codegen_object_equals_argument_boxing_3443,
           every_spec_5_1_value_kind_boxes_as_an_equals_argument_spec_8_3) {
    auto r = runAot(prog(
        "    var ro: Object := new Pt(1, 2); "
        "    Console.WriteLine(\"rec \" + ro.Equals(new Pt(1, 2)).ToString() "
        "                      + ro.Equals(new Pt(3, 4)).ToString()); "
        "    var eo: Object := Color.Red; "
        "    Console.WriteLine(\"enum \" + eo.Equals(Color.Red).ToString() "
        "                      + eo.Equals(Color.Green).ToString()); "
        "    var bo: Object := true; "
        "    Console.WriteLine(\"bool \" + bo.Equals(true).ToString() "
        "                      + bo.Equals(false).ToString()); "
        "    var dobj: Object := 1.5; "
        "    Console.WriteLine(\"dbl \" + dobj.Equals(1.5).ToString() "
        "                      + dobj.Equals(2.5).ToString()); "
        "    var co: Object := 'A'; "
        "    Console.WriteLine(\"chr \" + co.Equals('A').ToString() "
        "                      + co.Equals('B').ToString()); "
        "    var arr: [2]int := [1, 2]; "
        "    var same: [2]int := [1, 2]; "
        "    var diff: [2]int := [9, 9]; "
        "    var aobj: Object := arr; "
        "    Console.WriteLine(\"arr \" + aobj.Equals(same).ToString() "
        "                      + aobj.Equals(diff).ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("rec TrueFalse\nenum TrueFalse\n"
                                "bool TrueFalse\ndbl TrueFalse\n"
                                "chr TrueFalse\narr TrueFalse\n"));
}

// ── T3 — §8.6 *Nullable value-type boxing*, both arms. A present `int?` boxes to
// the NON-nullable `int` ("The nullable wrapper is stripped during boxing"), so
// it compares equal to the boxed 7 in `o`; an absent one boxes to "a null
// `Object` reference", which a non-null receiver is not equal to. Pre-fix both
// rows were dropped, so the two `int?` values were indistinguishable by
// omission.
BEZEL_TEST(codegen_object_equals_argument_boxing_3443,
           nullable_value_argument_boxes_per_spec_8_6_nullable_rule) {
    auto r = runAot(prog(
        "    var o: Object := 7; "
        "    var n: int? := 7; "
        "    var m: int? := null; "
        "    Console.WriteLine(\"n \" + o.Equals(n).ToString()); "
        "    Console.WriteLine(\"m \" + o.Equals(m).ToString()); "
        "    Console.WriteLine(\"lit \" + o.Equals(null).ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("n True\nm False\nlit False\n"));
}

// ── T4 — the argument is evaluated EXACTLY ONCE (§22.3's "evaluated exactly
// once"). `Mark` prints and returns its input, so a second evaluation prints a
// second marker line and a hoisted one prints in the wrong place. Pre-fix the
// whole statement — marker included — was gone.
BEZEL_TEST(codegen_object_equals_argument_boxing_3443,
           equals_argument_evaluated_exactly_once_spec_22_3) {
    auto r = runAot(prog(
        "    var o: Object := 7; "
        "    Console.WriteLine(\"before\"); "
        "    Console.WriteLine(\"r \" + o.Equals(C.Mark(7)).ToString()); "
        "    Console.WriteLine(\"after\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("before\narg7\nr True\nafter\n"));
}

// ── T5 — the `?.` MIRROR, which is a separate lowering site and was broken
// identically. §7.5: "`?.` short-circuits. The receiver is evaluated first; if
// it is null the access does not occur and nothing to its right in the chain is
// evaluated" — so the null-receiver row must NOT print `Mark`'s marker, which is
// exactly what pins that the new boxing sits INSIDE the non-null arm and does
// not hoist the argument out of the short circuit.
// (`.ToString()` is not called on the `bool?` result: that spelling emits
// malformed IR for every nullable value type, a pre-existing defect independent
// of this one — filed separately. `?? false` narrows first.)
BEZEL_TEST(codegen_object_equals_argument_boxing_3443,
           null_conditional_equals_boxes_and_short_circuits_spec_7_5_8_6) {
    auto r = runAot(prog(
        "    var on: Object? := 7; "
        "    var oz: Object? := null; "
        "    Console.WriteLine(\"p \" + (on?.Equals(7) ?? false).ToString()); "
        "    Console.WriteLine(\"q \" + (on?.Equals(5) ?? true).ToString()); "
        "    Console.WriteLine(\"z \" + "
        "        (oz?.Equals(C.Mark(9)) ?? false).ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("p True\nq False\nz False\n"));
}

// ── T6 — the BOXED-UNIT receiver, where this fix and #3422 meet. `var u: Object
// := ()` is §4.7's "produces a boxed `()`"; the argument `()` must box too
// (§5.1 lists the unit type) and then the unit box's own `Equals` thunk answers.
// §8.6's unit clause makes the reference question undecidable on purpose
// ("Programs should not rely on boxed unit values being either reference-equal
// or reference-distinct"), which is precisely why the answer must come from the
// vtable's value test and not from pointer identity.
BEZEL_TEST(codegen_object_equals_argument_boxing_3443,
           boxed_unit_receiver_and_unit_argument_spec_8_6_4_7) {
    auto r = runAot(prog(
        "    var u: Object := (); "
        "    var o: Object := 7; "
        "    Console.WriteLine(\"uu \" + u.Equals(()).ToString()); "
        "    Console.WriteLine(\"ui \" + u.Equals(5).ToString()); "
        "    Console.WriteLine(\"iu \" + o.Equals(()).ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("uu True\nui False\niu False\n"));
}

// ── T7 — the two OTHER receiver arms this fix touches: an INTERFACE-typed
// receiver and a plain CLASS receiver with no `Equals` override (§8.3's Class
// row, "Reference equality"). Both take the same code path as the `Object`
// receiver and both dropped a value-typed argument pre-fix. The reference-
// identity answers also pin that boxing the argument did not accidentally turn
// a class comparison into a value one.
BEZEL_TEST(codegen_object_equals_argument_boxing_3443,
           interface_and_class_receivers_box_the_argument_spec_8_3_8_6) {
    auto r = runAot(prog(
        "    using var sq := new Sq(3); "
        "    var ish: IShape := sq; "
        "    Console.WriteLine(\"i1 \" + ish.Equals(sq).ToString()); "
        "    Console.WriteLine(\"i2 \" + ish.Equals(5).ToString()); "
        "    using var p1 := new Plain(); "
        "    using var p2 := new Plain(); "
        "    Console.WriteLine(\"c1 \" + p1.Equals(p1).ToString()); "
        "    Console.WriteLine(\"c2 \" + p1.Equals(p2).ToString()); "
        "    Console.WriteLine(\"c3 \" + p1.Equals(5).ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("i1 True\ni2 False\nc1 True\nc2 False\n"
                                "c3 False\n"));
}

// ── T8 — CONTROL: the REFERENCE-typed arguments, which already worked and must
// be byte-identical. §5.1 converts these to `Object` "directly", so
// `boxForTarget` must decline for every one of them — a fix that boxed a
// reference source would wrap a class pointer in a box and turn `True` into
// `False` here.
BEZEL_TEST(codegen_object_equals_argument_boxing_3443,
           reference_typed_arguments_are_not_boxed_spec_5_1) {
    auto r = runAot(prog(
        "    using var p := new Plain(); "
        "    var po: Object := p; "
        "    var s: string := \"x\"; "
        "    var so: Object := s; "
        "    Console.WriteLine(\"r1 \" + po.Equals(p).ToString()); "
        "    Console.WriteLine(\"r2 \" + po.Equals(po).ToString()); "
        "    Console.WriteLine(\"r3 \" + so.Equals(s).ToString()); "
        "    Console.WriteLine(\"r4 \" + so.Equals(\"x\").ToString()); "
        "    Console.WriteLine(\"r5 \" + so.Equals(po).ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("r1 True\nr2 True\nr3 True\nr4 True\n"
                                "r5 False\n"));
}
