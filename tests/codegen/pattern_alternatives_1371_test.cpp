// Codegen value tests for v0.167 (#1371) pattern alternatives spelled `|` —
// §19.3 / Appendix A.7 / A.6. Alternatives lower to an OR of per-member tests
// (the same ConstantPattern lowering used before, now reachable per positional
// slot too). Observable via the int exit code.
//
// Covered:
//   - a `|` arm selects for its first AND last member,
//   - a per-slot `|` alternative inside a record destructuring selects,
//   - a `catch A | B` arm catches both types and the binding has the nearest
//     common base type (§31.2.1) — verified by calling a method typed on the
//     base through the bound variable,
//   - a pattern-embedded `|` parses below Shift, so a range/relational arm with
//     arithmetic bounds coexists with a constant `|` alternatives arm.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int runProg(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// A `case` over an int classifier returning distinct codes.
int classify(const std::string& scrut) {
    return runProg(
        std::string("module M; class C public static method Main(): int ") +
        "  var x := " + scrut + "; "
        "  return (case x of "
        "    1 | 2 | 3 => 10, "
        "    7 | 8 | 9 => 20, "
        "    _ => 30); end Main; end C;");
}

} // namespace

// ---- `|` arm selects for first and last member -----------------------------

BEZEL_TEST(codegen_alt_1371, arm_first_member) {
    BEZEL_EXPECT_EQ(classify("1"), 10);   // first member of `1 | 2 | 3`
}

BEZEL_TEST(codegen_alt_1371, arm_last_member) {
    BEZEL_EXPECT_EQ(classify("3"), 10);   // last member of `1 | 2 | 3`
}

BEZEL_TEST(codegen_alt_1371, arm_second_alt_first_member) {
    BEZEL_EXPECT_EQ(classify("7"), 20);   // first member of `7 | 8 | 9`
}

BEZEL_TEST(codegen_alt_1371, arm_second_alt_last_member) {
    BEZEL_EXPECT_EQ(classify("9"), 20);   // last member of `7 | 8 | 9`
}

BEZEL_TEST(codegen_alt_1371, arm_no_member_falls_through) {
    BEZEL_EXPECT_EQ(classify("5"), 30);   // matches neither list → `_`
}

// ---- per-slot `|` alternative inside a record destructuring ----------------

BEZEL_TEST(codegen_alt_1371, per_slot_alternative_selects) {
    // Pair(color, n): the first slot alternates over two enum constants; the
    // second binds. Red and Blue take the first arm; Green falls through.
    const std::string base =
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "record Pair(a: Color, b: int); "
        "class C "
        "  public static method Describe(p: Pair): int "
        "    return (case p of "
        "      Pair(Color.Red | Color.Blue, var x) => x, "
        "      Pair(Color.Green, _) => 100, "
        "      _ => 200); "
        "  end Describe; ";
    BEZEL_EXPECT_EQ(runProg(base +
        "  public static method Main(): int "
        "    return Describe(new Pair(Color.Red, 5)); end Main; end C;"), 5);
    BEZEL_EXPECT_EQ(runProg(base +
        "  public static method Main(): int "
        "    return Describe(new Pair(Color.Blue, 9)); end Main; end C;"), 9);
    BEZEL_EXPECT_EQ(runProg(base +
        "  public static method Main(): int "
        "    return Describe(new Pair(Color.Green, 5)); end Main; end C;"), 100);
}

// ---- `catch A | B` catches both types; binding has the common base ---------

BEZEL_TEST(codegen_alt_1371, catch_multi_type_catches_both) {
    // A2 and A3 both derive from A1. `catch var e: A2 | A3` binds `e` as the
    // nearest common base A1; calling A1.Code() through it proves the static
    // type is the base. Throwing A2 or A3 both hit the arm.
    const std::string base =
        "module M; "
        "class A1: Exception "
        "  public virtual method Code(): int => 1; "
        "end A1; "
        "class A2: A1 public override method Code(): int => 2; end A2; "
        "class A3: A1 public override method Code(): int => 3; end A3; "
        "class C ";
    // Throw A2 → caught, e.Code() dispatches to A2 → 2.
    BEZEL_EXPECT_EQ(runProg(base +
        "  public static method Main(): int "
        "    try throw new A2(); "
        "    catch var e: A2 | A3 => return e.Code(); end; "
        "    return 0; end Main; end C;"), 2);
    // Throw A3 → caught by the same arm → 3.
    BEZEL_EXPECT_EQ(runProg(base +
        "  public static method Main(): int "
        "    try throw new A3(); "
        "    catch var e: A2 | A3 => return e.Code(); end; "
        "    return 0; end Main; end C;"), 3);
}

BEZEL_TEST(codegen_alt_1371, catch_multi_type_no_binding_catches_both) {
    const std::string base =
        "module M; "
        "class E1: Exception end E1; "
        "class E2: Exception end E2; "
        "class C ";
    BEZEL_EXPECT_EQ(runProg(base +
        "  public static method Boom(n: int) "
        "    if n = 1 then throw new E1(); else throw new E2(); end; "
        "  end Boom; "
        "  public static method Try(n: int): int "
        "    try Boom(n); catch E1 | E2 => return 42; end; return 0; "
        "  end Try; "
        "  public static method Main(): int "
        "    return Try(1) + Try(2); end Main; end C;"), 84);
}

// ---- pattern-embedded `|` parses below Shift (no bitwise-or collision) ------

BEZEL_TEST(codegen_alt_1371, pipe_below_shift_with_range_and_relational) {
    // Arithmetic range/relational bounds coexist with a `|` constant list in
    // the same `case`; the `|` is the alternative separator, not bitwise-or.
    const std::string prog =
        "module M; class C "
        "  public static method Grade(score: int, base_: int): int "
        "    return (case score of "
        "      ..< base_ + 10 => 1, "        // leading range, Shift-level bound
        "      100 | 101 => 5, "             // constant `|` alternatives
        "      > base_ * 3 => 9, "           // relational, Shift-level operand
        "      _ => 0); "
        "  end Grade; "
        "  public static method Main(): int "
        "    Console.WriteLine(Grade(5, 10) * 1000 + Grade(100, 10) * 100 "
        "         + Grade(101, 10) * 10 + Grade(200, 10)); return 0; "
        "  end Main; end C;";
    // Grade(5,10): 5 < 20 → 1. Grade(100,10): not <20, is 100|101 → 5.
    // Grade(101,10): not <20, is 100|101 → 5. Grade(200,10): >30 → 9.
    // 1*1000 + 5*100 + 5*10 + 9 = 1559. 1559 > 255, so it cannot round-trip an
    // 8-bit exit code — observe it on stdout (runAotOut) instead (WriteLine
    // appends '\n').
    BEZEL_EXPECT_EQ(runAotOut(prog), "1559");
}
