// Behavior tests for the §17.15 binding atom `(var x := E; body)` (v0.216,
// issue #2098) — evaluation order, scoping, typing, and composition, driven
// through the real AOT pipeline (runAot).
//
// §17.15: "Each binding is initialized exactly once, at its clause, by
// ordinary evaluation in textual order — clauses first, body last, every
// clause on every evaluation"; "A binding is in scope from the end of its own
// clause: the subsequent clauses' initializers and the body see it"; "the
// atom's value, type, and ownership classification are the body's"; "It is
// self-delimited and composes in any operand position."

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return -999;
    return r.exitCode;
}
} // namespace

// §17.15 example 1: `var area := (var w := bounds.Width(); w * w);` — spelled
// with a literal-backed width so the value is pinned (6*6 = 36).
BEZEL_TEST(bind_atom_behavior_2098, spec_17_15_example_area) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "public static method Width(): int return 6; end Width; "
        "public static method Main(): int "
        "  var area := (var w := C.Width(); w * w); "
        "  return area; "
        "end Main; end C;"), 36);
}

// §17.15: clauses chain and each later clause's initializer sees the earlier
// bindings — `(var a := 2; var b := a + 3; a * b)` = 2 * 5 = 10.
BEZEL_TEST(bind_atom_behavior_2098, spec_17_15_clause_chain_scoping) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (var a := 2; var b := a + 3; a * b); "
        "end Main; end C;"), 10);
}

// §17.15: "the type is ascribed (with typed-context widening, §5.1)" — a
// clause ascription types exactly as the §21.1 declaration it is spelled as.
BEZEL_TEST(bind_atom_behavior_2098, spec_17_15_ascribed_clause_widening) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var v := (var n: long := 40; n + 2L); "
        "  return int(v); "
        "end Main; end C;"), 42);
}

// §17.15: the atom composes in any operand position — as a call argument
// (supplying its own parens) and as a binary operand.
BEZEL_TEST(bind_atom_behavior_2098, spec_17_15_operand_positions) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "public static method F(x: int): int return x * 2; end F; "
        "public static method Main(): int "
        "  return F((var n := 3; n + 1)) + (var m := 5; m * 2); "
        "end Main; end C;"), 18);
}

// §17.15: nested atoms — a clause initializer may itself be an atom; each
// atom's bindings scope over its own later clauses and body only.
BEZEL_TEST(bind_atom_behavior_2098, spec_17_15_nested_atoms) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (var a := (var b := 4; b + b); a * 3); "
        "end Main; end C;"), 24);
}

// §17.15 example 2: a v0.208 paren atom as the atom's body —
// `(var n := items.Count; (if n = 1 then … else …))`.
BEZEL_TEST(bind_atom_behavior_2098, spec_17_15_if_atom_body) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (var n := 1; (if n = 1 then 11 else 22)); "
        "end Main; end C;"), 11);
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (var n := 3; (if n = 1 then 11 else 22)); "
        "end Main; end C;"), 22);
}

// §17.15: shadowing — "a binding whose name is already in scope … follows
// the same duplicate-declaration rule as a §21.1 local in a nested scope";
// the atom's binding shadows the outer local inside the atom only, and the
// outer local is unchanged after the atom (single-assignment: the atom
// cannot write it).
BEZEL_TEST(bind_atom_behavior_2098, spec_17_15_shadow_outer_local) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := 5; "
        "  var v := (var n := 100; n + 1); "
        "  return v + n; "  // 101 + 5
        "end Main; end C;"), 106);
}

// §17.15 / §22.16 v0.216: the motivating position — the atom in a contract
// clause names a subresult instead of recomputing it
// (`requires((var m := n + n; m >= n and m <= _capacity))`). The clause
// passes for a satisfying argument and the method computes normally.
BEZEL_TEST(bind_atom_behavior_2098, spec_22_16_contract_clause_atom) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter "
        "  private _capacity: int := 0; "
        "  public constructor(cap: int) _capacity := cap; end constructor; "
        "  public method Twice(n: int): int "
        "    requires((var m := n + n; m >= n and m <= _capacity)) "
        "    return n + n; "
        "  end Twice; "
        "end Counter; "
        "class C public static method Main(): int "
        "  using var c := new Counter(100); "
        "  return c.Twice(3); "
        "end Main; end C;"), 6);
}

// §17.15 / §21.9: in statement position the atom is an ordinary expression
// statement under the discard regime — the discard form `_ := (…);` stands.
BEZEL_TEST(bind_atom_behavior_2098, spec_21_9_statement_position) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "private static _hits: int := 0; "
        "public static method G(n: int): int "
        "  C._hits := C._hits + n; return n; "
        "end G; "
        "public static method Main(): int "
        "  _ := (var n := 21; C.G(n) + C.G(n)); "
        "  return C._hits; "
        "end Main; end C;"), 42);
}
