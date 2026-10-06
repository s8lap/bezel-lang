// Behavior tests for the v0.208 paren-delimited expression forms — spec §20 /
// §19.1 / §19.7 / §9.5 / §30.7 (proposal 0057 graduation; impl #2079).
//
// v0.208's contract is "semantics unchanged everywhere": the `(if …)` atom
// selects exactly as the retired postfix conditional did (only the selected
// branch evaluates; §20 common-type rule; v0.210 bare-`null` branch typing),
// the `(case …)`/`(when …)` atoms carry §19's arm semantics, and `with(…)`
// keeps §9.5's copy-not-construct semantics. Each case FAILS without the
// #2079 parser change (pre-cutover, `(if` was an unresolvable parse) and
// PASSES with it.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// §20 v0.208 (the spec's own `max` example): two-branch selection.
BEZEL_TEST(paren_forms_2079, spec_20_if_atom_max) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "var a := 40; var b := 2; "
        "var max := (if a > b then a else b); "
        "return max + b; "
        "end Main; end C;"), 42);
}

// §20 v0.208 (the spec's own grade example): the elif chain, first-true
// selection, in textual order.
BEZEL_TEST(paren_forms_2079, spec_20_if_atom_elif_grade) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "public static method Grade(score: int): int "
        "return (if score >= 90 then 1 elif score >= 80 then 2 "
        "elif score >= 70 then 3 else 4); "
        "end Grade; "
        "public static method Main(): int "
        "if Grade(95) <> 1 then return 1; end; "
        "if Grade(85) <> 2 then return 2; end; "
        "if Grade(75) <> 3 then return 3; end; "
        "if Grade(65) <> 4 then return 4; end; "
        "return 0; "
        "end Main; end C;"), 0);
}

// §20 v0.210 (bare-`null` branch): `(if c then s else null)` types to the
// nullable form of the other branch's type.
BEZEL_TEST(paren_forms_2079, spec_20_if_atom_null_branch) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "public static method Pick(c: bool): string? "
        "return (if c then \"s\" else null); "
        "end Pick; "
        "public static method Main(): int "
        "if Pick(true) is null then return 1; end; "
        "if Pick(false) is not null then return 2; end; "
        "return 0; "
        "end Main; end C;"), 0);
}

// §20 v0.208: a `throw` branch is admitted in any branch position and is
// bottom-typed (the v0.182 true-branch trap is deleted).
BEZEL_TEST(paren_forms_2079, spec_20_if_atom_throw_branch) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "public static method Guarded(n: int): int "
        "return (if n > 0 then n else throw new ArgumentException(\"neg\")); "
        "end Guarded; "
        "public static method Main(): int "
        "if Guarded(5) <> 5 then return 1; end; "
        "try _ := Guarded(-1); return 2; "
        "catch ArgumentException => return 0; end; "
        "return 3; "
        "end Main; end C;"), 0);
}

// §19.1 v0.208 (the spec's own grade examples): the case atom produces the
// selected arm's value.
// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `(when …)`
// expression is retired — the when atom is removed; the case atom remains.
BEZEL_TEST(paren_forms_2079, spec_19_1_case_and_when_atoms) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "var score := 85; "
        "var g1 := (case score of 90..100 => 1, 80..89 => 2, 70..<80 => 3, "
        "_ => 4); "
        "if g1 <> 2 then return 1; end; "
        "return 2; "  // was: check g2, return 0 — g2 retired at v0.289
        "end Main; end C;"), 2);
}

// §19.7 v0.208: in call-argument position the atom's parentheses are its own
// (`f((case x of …), y)`).
BEZEL_TEST(paren_forms_2079, spec_19_7_atom_in_argument) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "public static method F(a: int, b: int): int return a * b; end F; "
        "public static method Main(): int "
        "return F((case 7 of 7 => 6, _ => 1), (if true then 7 else 0)); "
        "end Main; end C;"), 42);
}

// §19 v0.208: the statement forms are unchanged — `end`-closed, statement
// arm bodies, value discarded per §21.9.
// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when … end when;`
// statement is retired — removed; the case statement arm remains as the subject.
BEZEL_TEST(paren_forms_2079, spec_19_7_statement_forms_run) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "var total := 0; "
        "case 2 of 1 => total := 1;, 2 => do total := 40; total := total + 2; "
        "end, _ => total := 9; end case; "
        "return total; "
        "end Main; end C;"), 42);
}

// §9.5 v0.208: `with(…)` keeps copy-with-overrides semantics; the suffix
// chains past its `)`.
BEZEL_TEST(paren_forms_2079, spec_9_5_with_parens_semantics) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "var p := new Point(1, 2); "
        "var q := p with(X := 10); "
        "if p.X <> 1 then return 1; end; "
        "if q.X <> 10 or q.Y <> 2 then return 2; end; "
        "var r := p with(X := 20) with(Y := 22); "
        "return r.X + r.Y; "
        "end Main; end C;"), 42);
}

// §9.5/§30.7 v0.208: header-`with` followed by `( Identifier :=` is the
// update suffix even inside a `for` header (the content-based resolution the
// spec states) — here the suffix applies inside the header's collection
// literal.
BEZEL_TEST(paren_forms_2079, spec_30_7_header_with_is_update_suffix) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pair(A: int, B: int); "
        "class C public static method Main(): int "
        "var p := new Pair(1, 2); "
        "var total := 0; "
        "for var x in [p with(A := 40), p with(A := 2)] do "
        "total := total + x.A; end; "
        "return total; "
        "end Main; end C;"), 42);
}
