// fix #3422 — `u.Equals(x)` and `u.GetHashCode()` on a `()` RECEIVER did not
// lower at all, so the enclosing statement was dropped whole (spec §8.1
// *Definition*, §8.3 *`Equals` and `GetHashCode`*, §8.5 *`ToString`*,
// §8.6 *Boxing*, §4.7 *The unit type*, §22.3 / §22.5 — v0.283).
//
// MEASURED on the parent commit, with the issue's repro:
//
//     var u: () := ();
//     Console.WriteLine(u.Equals(u).ToString());
//     Console.WriteLine(u.GetHashCode().ToString());
//
//   :5:23: internal: the argument of this `Console.WriteLine` failed to lower
//          to a value, so the whole statement emits nothing — not even the line
//          terminator (stdlib §2.1 / §21.9) — #3079
//   :6:23: internal: … (same)
//
//   stdout: (empty)      exit: 0
//
// Bound to a local instead of printed, the same two draw the `#2434` note and
// the slot is zero-initialized — `False` and `0`, a WRONG VALUE with only an
// `internal:` note to show for it.
//
// ── WHAT THE ANSWER IS, AND WHERE IT COMES FROM ──────────────────────────────
//
// §8.1 declares the four universals on `Object` and states "`Object` is the
// universal root. Every type derives from it: … records, enums, primitives,
// inline arrays, and the unit type derive via boxing ([§8.6]) when used in
// `Object`-typed positions". §8.5 (v0.252) states the universality outright:
// "The defaults above are generated for **every** type of the applicable kind,
// wherever declared". So SOME answer is mandatory; `u.ToString()` already gave
// §8.5's `"()"` and only these two were missing.
//
// §8.3's own table has **no unit row** — this is a reading the document does
// not state, and it is the SAME reading the compiler already committed to for
// the BOXED form in #2915 (`ensureUnitBoxVtable`'s `bz$uniteq` / `bz$unithash`,
// whose comment records it): the unit type has exactly one value, so §8.3's
// Primitive-row "Value comparison" can only mean "the other operand is also a
// unit", and "Value-based" hashing of a one-valued type can only be a constant.
// §8.3's invariant — "if `a.Equals(b)` then `a.GetHashCode() = b.GetHashCode()`"
// — then holds by construction rather than by agreement between two
// computations.
//
// THAT INVARIANT IS WHY THE UNBOXED ANSWER IS NOT FREE TO DIFFER FROM THE BOXED
// ONE. `u.Equals(o)` is true for a boxed-`()` `o` (T4 below), so §8.3 requires
// `u.GetHashCode() = o.GetHashCode()`. T2 asserts exactly that equality rather
// than a magic number, so the test states the SPEC's requirement and not the
// implementation's choice of constant — any constant satisfies §8.3 here.
//
// ── WHAT MAKES EACH CASE ABLE TO FAIL ────────────────────────────────────────
//
// Pre-fix the program EXITS 0, so "it ran" is not an oracle; and the degraded
// local form yields `False` / `0`, so an oracle that accepts those is not one
// either. Every row therefore asserts exact stdout, T2 additionally pins the
// hash NON-ZERO (0 is what every dropped path already produces — #2804's trap),
// and T5 pins the FALSE cases so a fix that answered `True` unconditionally
// fails.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

constexpr const char* kDecls =
    "module M3422; "
    "class C "
    "  public static method Nothing() "
    "    Console.WriteLine(\"side\"); "
    "  end Nothing; ";

std::string prog(const std::string& body) {
    return std::string(kDecls) +
           "  public static method Main(): int " + body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ── T1 — THE ISSUE'S EXACT REPRO. Pre-fix: two `#3079` notes and an EMPTY
// stdout. Post-fix both statements lower and print.
BEZEL_TEST(codegen_unit_receiver_equals_hashcode_3422,
           unit_receiver_equals_and_hashcode_lower_spec_8_1_8_3) {
    auto r = runAot(prog(
        "    var u: () := (); "
        "    Console.WriteLine(u.Equals(u).ToString()); "
        "    Console.WriteLine((u.GetHashCode() <> 0).ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nTrue\n"));
}

// ── T2 — §8.3's INVARIANT across the boxed and unboxed spellings of the same
// value. `u.Equals(o)` is true for a boxed `()` (T4), so §8.3's "if
// `a.Equals(b)` then `a.GetHashCode() = b.GetHashCode()`" makes the two hashes
// equal, and MUST: the boxed form already answered a constant before this fix
// (#2915's `bz$unithash`), so an unboxed answer picked independently would
// break the invariant silently. The NON-ZERO assertion is what stops the
// degenerate `0` both sides produced pre-fix from passing this row (#2804).
BEZEL_TEST(codegen_unit_receiver_equals_hashcode_3422,
           unboxed_and_boxed_unit_hashes_agree_spec_8_3_8_6) {
    auto r = runAot(prog(
        "    var u: () := (); "
        "    var o: Object := (); "
        "    var hu := u.GetHashCode(); "
        "    var ho := o.GetHashCode(); "
        "    Console.WriteLine((hu = ho).ToString()); "
        "    Console.WriteLine((hu <> 0).ToString()); "
        "    Console.WriteLine((hu = u.GetHashCode()).ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nTrue\nTrue\n"));
}

// ── T3 — the #3431 rule at THESE members: the receiver is an evaluated operand
// (§22.3 "the receiver … evaluated exactly once"), and the argument runs too
// (§22.5 "observable … and is normative"). Pre-fix neither ran, because the
// whole call declined. The markers pin BOTH effects and their order.
BEZEL_TEST(codegen_unit_receiver_equals_hashcode_3422,
           unit_receiver_and_argument_effects_run_spec_22_3_22_5) {
    auto r = runAot(prog(
        "    Console.WriteLine(\"A\"); "
        "    var b := C.Nothing().Equals(()); "
        "    Console.WriteLine(\"B\" + b.ToString()); "
        "    var h := C.Nothing().GetHashCode(); "
        "    Console.WriteLine(\"C\" + (h <> 0).ToString()); "
        "    var u: () := (); "
        "    var d := u.Equals(C.Nothing()); "
        "    Console.WriteLine(\"D\" + d.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("A\nside\nBTrue\nside\nCTrue\n"
                                "side\nDTrue\n"));
}

// ── T4 — the TRUE cases, across the spellings a unit value reaches an
// `Object?` parameter by. §8.6 "Boxing of the unit value": "The unit type `()`
// has exactly one value, so all boxings of `()` may share a single heap
// object", and §8.6's nullable rule — "If the value is non-null, the result is
// a boxed `T` (the non-nullable form). The nullable wrapper is stripped during
// boxing" — is what makes a PRESENT `()?` argument equal too.
BEZEL_TEST(codegen_unit_receiver_equals_hashcode_3422,
           unit_equals_every_unit_spelling_spec_8_3_8_6) {
    auto r = runAot(prog(
        "    var u: () := (); "
        "    var v: () := (); "
        "    var o: Object := (); "
        "    var p: ()? := (); "
        "    Console.WriteLine(u.Equals(u).ToString()); "
        "    Console.WriteLine(u.Equals(v).ToString()); "
        "    Console.WriteLine(u.Equals(()).ToString()); "
        "    Console.WriteLine(u.Equals(o).ToString()); "
        "    Console.WriteLine(u.Equals(p).ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("True\nTrue\nTrue\nTrue\nTrue\n"));
}

// ── T5 — the FALSE cases. Without these, a fix that returned a constant `True`
// passes every row above. §8.4: "`null = nonNull` … `false`" (and §8.1's formal
// is `Object?`, so `null` is an admissible argument); an `int`, a `string` and
// a class instance each box or reference to something that is not the unit box,
// and a NULL `()?` boxes to "a null `Object` reference" (§8.6's nullable rule).
BEZEL_TEST(codegen_unit_receiver_equals_hashcode_3422,
           unit_equals_nothing_else_spec_8_3_8_4_8_6) {
    auto r = runAot(
        "module M3422B; "
        "class W end W; "
        "class C "
        "  public static method Main(): int "
        "    var u: () := (); "
        "    var z: ()? := null; "
        "    var oi: Object := 7; "
        "    using var w := new W(); "
        "    Console.WriteLine(u.Equals(null).ToString()); "
        "    Console.WriteLine(u.Equals(5).ToString()); "
        "    Console.WriteLine(u.Equals(\"x\").ToString()); "
        "    Console.WriteLine(u.Equals(oi).ToString()); "
        "    Console.WriteLine(u.Equals(w).ToString()); "
        "    Console.WriteLine(u.Equals(z).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("False\nFalse\nFalse\nFalse\nFalse\nFalse\n"));
}

// ── T6 — CONTROL: the BOXED unit's own universals, which #2915 made work and
// this fix must leave alone. `bz$uniteq` is now emitted from the same helper
// the unboxed path calls, so a mistake in the extraction shows up here rather
// than only in the new rows.
BEZEL_TEST(codegen_unit_receiver_equals_hashcode_3422,
           boxed_unit_universals_unchanged_spec_8_6_2915) {
    auto r = runAot(prog(
        "    var o: Object := (); "
        "    var q: Object := (); "
        "    var oi: Object := 7; "
        "    Console.WriteLine(o.ToString()); "
        "    Console.WriteLine(o.Equals(q).ToString()); "
        "    Console.WriteLine(o.Equals(oi).ToString()); "
        "    Console.WriteLine((o.GetHashCode() = q.GetHashCode()).ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("()\nTrue\nFalse\nTrue\n"));
}

// ── T7 — CONTROL: `u.ToString()` — the one universal that already worked on a
// `()` receiver — still answers §8.5's `"()"`, in both the local and the
// unit-valued-call spellings. The three universals share one arm after this
// fix, so ToString is the row that proves the widening did not displace it.
BEZEL_TEST(codegen_unit_receiver_equals_hashcode_3422,
           unit_tostring_unchanged_spec_8_5) {
    auto r = runAot(prog(
        "    var u: () := (); "
        "    Console.WriteLine(u.ToString()); "
        "    Console.WriteLine(C.Nothing().ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("()\nside\n()\n"));
}
