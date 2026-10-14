// Codegen / §21.5 (v0.338) — #6139: `break` / `continue` inside a `for` whose
// source is an INLINE collection literal (`for var i in [0, 1, 2] do … end`).
//
// Spec §21.5 (v0.338): "`break` exits a loop; `continue` skips the rest of the
// current iteration and advances to the next. The unlabeled forms target the
// immediately enclosing loop." And for the labeled forms: "`continue name;`
// transfers control to the labeled loop's *advance point*: … the index/step
// increment (which then falls into the bound check) on the direct-indexed and
// counted forms."
//
// ROOT CAUSE (src/codegen/LlvmBackend_Stmts.cpp, Emitter::lowerArrayFor). The
// collection-literal arm carried its own hand-rolled copy of the indexed loop
// (`clit.cond` / `clit.body` / `clit.end`) that pushed NO `loopStack_` frame,
// had no increment block, and ignored lowerForBody's "body terminated" result.
// So a jump in the body
//   * found an empty loop stack and was dropped — the rest of the iteration ran
//     and the loop went on (silent wrong results; and since §7.4 early-exit
//     narrowing trusts the `break`, a null dereference: SEGV);
//   * or found the frame of an ENCLOSING loop and jumped out of THAT one — a
//     `break` meant for the literal loop left the surrounding `while`;
//   * and, when unconditional, left the loop's back-edge after a terminator
//     ("Terminator found in the middle of a basic block!").
// FIX: the literal arm only materialises the temp array and then shares the one
// direct-indexed loop a named inline array uses (frame, increment block,
// terminator check).
//
// Every row is on runAotChecked (generated-code ASan/LSan) and asserts stdout.
// Fail-without: every row not marked CONTROL fails on the pre-fix compiler; the
// CONTROL rows pin the neighbouring shapes that were already right and must
// stay right.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — the AOT oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// The program's stdout with the trailing line ending removed, or a marker that
// can never equal an expectation when the program was rejected, crashed, tripped
// the sanitizer or exited non-zero.
std::string out(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did not run clean>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    std::string s = r.stdoutText;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// `module M; <decls> class P <members> Main() { <body> return 0; } end P;`
std::string prog(const std::string& body, const std::string& members = "",
                 const std::string& decls = "") {
    return "module M; " + decls + " public class P " + members +
           " public static method Main(): int " + body +
           " return 0; end Main; end P;";
}

// A class whose live-instance count is observable: constructor +1, destructor -1.
const char* const kLive =
    "public class D "
    "  public static Live: int := 0; "
    "  public property V: int; "
    "  public constructor(v: int) V := v; D.Live := D.Live + 1; end constructor; "
    "  destructor D.Live := D.Live - 1; end destructor; "
    "end D; ";

}  // namespace

// ===========================================================================
// The issue's programs.
// ===========================================================================

// §21.5 (#6139): the issue's repro, verbatim. Before the fix it printed a fourth
// line `i=2 eq=False` and `t=3 u=3 w=1`.
BEZEL_TEST(for_inline_literal_break_continue_6139,
           issue_repro_break_exits_the_literal_loop_spec_21_5) {
    std::string src =
        "module M;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    var t := 0;\n"
        "    for var i in [0, 1, 2] do\n"
        "      Console.WriteLine($\"i={i} eq={i = 1}\");\n"
        "      if i = 1 then Console.WriteLine(\"hit\"); break; end if;\n"
        "      t := t + 1;\n"
        "    end for;\n"
        "    var u := 0;\n"
        "    for var j in [5, 6, 7] do\n"
        "      if true then break; end if;\n"
        "      u := u + 1;\n"
        "    end for;\n"
        "    var xs: Array<int> := [0, 1, 2]; var w := 0;\n"
        "    for var m in xs do\n"
        "      if m = 1 then break; end if;\n"
        "      w := w + 1;\n"
        "    end for;\n"
        "    Console.WriteLine($\"t={t} u={u} w={w}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n";
    BEZEL_EXPECT_EQ(out(src),
                    std::string("i=0 eq=False\ni=1 eq=True\nhit\nt=1 u=0 w=1"));
}

// §21.5 / §7.4 (#6139): the issue's memory-unsafety shape. §7.4's early-exit
// narrowing trusts `if s = null then break;`, so `s.Length` below it is read
// unguarded; with the `break` dropped the second iteration dereferenced null
// (AddressSanitizer: SEGV).
BEZEL_TEST(for_inline_literal_break_continue_6139,
           issue_early_exit_narrowing_break_no_null_deref_spec_21_5_7_4) {
    BEZEL_EXPECT_EQ(
        out(prog("var t := 0; var s: string? := \"ab\"; "
                 "for var i in [0, 1] do "
                 "  if s = null then break; end if; "
                 "  t := t + s.Length; s := null; "
                 "end for; "
                 "Console.WriteLine($\"t={t}\");")),
        std::string("t=2"));
}

// §21.5 / §7.4 (#6139): the same narrowing through `continue`.
BEZEL_TEST(for_inline_literal_break_continue_6139,
           early_exit_narrowing_continue_no_null_deref_spec_21_5_7_4) {
    BEZEL_EXPECT_EQ(
        out(prog("var t := 0; var s: string? := \"ab\"; "
                 "for var i in [0, 1, 2] do "
                 "  if s = null then continue; end if; "
                 "  t := t + s.Length; s := null; "
                 "end for; "
                 "Console.WriteLine($\"t={t}\");")),
        std::string("t=2"));
}

// §21.5 (#6139): the issue's verifier-failure shape — an unconditional `break`
// in a literal loop nested in a `while true`. Before the fix: "Terminator found
// in the middle of a basic block!" (no executable). The `break` exits the
// literal loop only, so the `while` runs its three rounds.
BEZEL_TEST(for_inline_literal_break_continue_6139,
           issue_unconditional_break_nested_in_while_compiles_spec_21_5) {
    BEZEL_EXPECT_EQ(
        out(prog("var k := 0; "
                 "while true do "
                 "  for var i in [0, 1] do break; end for; "
                 "  k := k + 1; "
                 "  if k > 2 then break; end if; "
                 "end while; "
                 "Console.WriteLine($\"{k}\");")),
        std::string("3"));
}

// ===========================================================================
// Unlabeled forms, by element kind.
// ===========================================================================

// §21.5 (#6139): `continue` advances to the next element. Before: t=6.
BEZEL_TEST(for_inline_literal_break_continue_6139,
           continue_skips_rest_of_iteration_int_spec_21_5) {
    BEZEL_EXPECT_EQ(
        out(prog("var t := 0; "
                 "for var i in [0, 1, 2, 3] do "
                 "  if i mod 2 = 0 then continue; end if; t := t + i; "
                 "end for; "
                 "Console.WriteLine($\"t={t}\");")),
        std::string("t=4"));
}

// §21.5 (#6139): unconditional jumps as the body's last statement. Before: 33.
BEZEL_TEST(for_inline_literal_break_continue_6139,
           unconditional_continue_and_break_spec_21_5) {
    BEZEL_EXPECT_EQ(
        out(prog("var k := 0; "
                 "for var i in [0, 1, 2] do k := k + 1; continue; end for; "
                 "for var i in [0, 1, 2] do k := k + 10; break; end for; "
                 "Console.WriteLine($\"{k}\");")),
        std::string("13"));
}

// §21.5 (#6139): string elements. Before: "abc|abc".
BEZEL_TEST(for_inline_literal_break_continue_6139,
           string_elements_break_and_continue_spec_21_5) {
    BEZEL_EXPECT_EQ(
        out(prog("var t := \"\"; var u := \"\"; "
                 "for var s in [\"a\", \"b\", \"c\"] do "
                 "  if s = \"b\" then break; end if; t := t + s; "
                 "end for; "
                 "for var s in [\"a\", \"b\", \"c\"] do "
                 "  if s = \"b\" then continue; end if; u := u + s; "
                 "end for; "
                 "Console.WriteLine($\"{t}|{u}\");")),
        std::string("a|ac"));
}

// §21.5 (#6139): double, bool and long (ascribed loop variable) elements.
// Before: "7.5 3 18".
BEZEL_TEST(for_inline_literal_break_continue_6139,
           double_bool_long_elements_spec_21_5) {
    BEZEL_EXPECT_EQ(
        out(prog("var d := 0.0; var b := 0; var l := 0L; "
                 "for var x in [1.5, 2.5, 3.5] do "
                 "  if x > 2.0 then break; end if; d := d + x; "
                 "end for; "
                 "for var x in [true, false, true] do "
                 "  if not x then break; end if; b := b + 1; "
                 "end for; "
                 "for var x: long in [5, 6, 7] do "
                 "  if x = 6L then continue; end if; l := l + x; "
                 "end for; "
                 "Console.WriteLine($\"{d} {b} {l}\");")),
        std::string("1.5 1 12"));
}

// §21.5 (#6139): nullable elements read from locals; the `break` fires on the
// null element. Before: t=3.
BEZEL_TEST(for_inline_literal_break_continue_6139,
           nullable_string_elements_break_on_null_spec_21_5) {
    BEZEL_EXPECT_EQ(
        out(prog("var a: string? := \"x\"; var b: string? := null; var t := 0; "
                 "for var s in [a, b, a] do "
                 "  if s = null then break; end if; t := t + 1; "
                 "end for; "
                 "Console.WriteLine($\"t={t}\");")),
        std::string("t=1"));
}

// §21.5 (#6139): record elements. Before: t=12.
BEZEL_TEST(for_inline_literal_break_continue_6139,
           record_elements_continue_spec_21_5) {
    BEZEL_EXPECT_EQ(
        out(prog("var t := 0; "
                 "for var r in [new R(1, 2), new R(3, 4), new R(5, 6)] do "
                 "  if r.A = 3 then continue; end if; t := t + r.B; "
                 "end for; "
                 "Console.WriteLine($\"t={t}\");",
                 "", "record R(A: int, B: int);")),
        std::string("t=8"));
}

// §21.5 (#6139): heap-array elements borrowed from named arrays. Before: t=8.
BEZEL_TEST(for_inline_literal_break_continue_6139,
           array_elements_break_spec_21_5) {
    BEZEL_EXPECT_EQ(
        out(prog("using var p: Array<int> := [1, 2]; "
                 "using var q: Array<int> := [3, 4]; var t := 0; "
                 "for var xs in [p, q, p] do "
                 "  if xs[0] = 3 then break; end if; t := t + xs[1]; "
                 "end for; "
                 "Console.WriteLine($\"t={t}\");")),
        std::string("t=2"));
}

// §21.5 / §28.3 (#6139): class elements BORROWED from `using var` owners, inside
// a `while` that runs 100 rounds. Each round: the first literal loop adds 1 and
// breaks, the second adds 1 + 3 and skips the middle element, so t = 500; every
// D is disposed exactly once by its `using var` (live = 0, no double-free under
// ASan). Before the fix the first `break` found the WHILE's frame and left the
// while during its first round (t=1).
BEZEL_TEST(for_inline_literal_break_continue_6139,
           class_elements_break_does_not_exit_enclosing_while_spec_21_5) {
    BEZEL_EXPECT_EQ(
        out(prog("var t := 0; var n := 0; "
                 "while n < 100 do "
                 "  using var a := new D(1); using var b := new D(2); "
                 "  using var c := new D(3); "
                 "  for var x in [a, b, c] do "
                 "    if x.V = 2 then break; end if; t := t + x.V; "
                 "  end for; "
                 "  for var x in [a, b, c] do "
                 "    if x.V = 2 then continue; end if; t := t + x.V; "
                 "  end for; "
                 "  n := n + 1; "
                 "end while; "
                 "Console.WriteLine($\"t={t} n={n} live={D.Live}\");",
                 "", kLive)),
        std::string("t=500 n=100 live=0"));
}

// ===========================================================================
// Literal spellings.
// ===========================================================================

// §21.5 (#6139): one element, an ascribed loop variable, a parenthesised
// literal, a trailing comma, and the ignore `_` as the loop variable (§21.4).
// Before: "11 3 3 3 3".
BEZEL_TEST(for_inline_literal_break_continue_6139,
           literal_spellings_spec_21_5) {
    BEZEL_EXPECT_EQ(
        out(prog("var a := 0; var b := 0; var c := 0; var d := 0; var e := 0; "
                 "for var i in [7] do if true then break; end if; a := a + 1; "
                 "end for; "
                 "for var i in [7] do if true then continue; end if; "
                 "  a := a + 10; end for; "
                 "for var i: int in [0, 1, 2] do "
                 "  if i = 1 then break; end if; b := b + 1; end for; "
                 "for var i in ([0, 1, 2]) do "
                 "  if i = 1 then break; end if; c := c + 1; end for; "
                 "for var i in [0, 1, 2,] do "
                 "  if i = 1 then break; end if; d := d + 1; end for; "
                 "for var _ in [1, 2, 3] do "
                 "  e := e + 1; if e = 2 then break; end if; end for; "
                 "Console.WriteLine($\"{a} {b} {c} {d} {e}\");")),
        std::string("0 1 1 1 2"));
}

// §21.5 (#6139): the source expression is evaluated once, before the loop — every
// element's call runs exactly once, and a `break` at the second element does not
// skip or repeat one ("A `continue` … never re-runs the source expression").
// Before, the first loop alone gave t=60 (F1 F2 F3 printed once each: the
// evaluation count was always right, the jump was not).
BEZEL_TEST(for_inline_literal_break_continue_6139,
           element_expressions_evaluated_once_spec_21_5) {
    BEZEL_EXPECT_EQ(
        out(prog("var t := 0; "
                 "for var i in [P.F(1), P.F(2), P.F(3)] do "
                 "  if i = 20 then break; end if; t := t + i; "
                 "end for; "
                 "var u := 0; "
                 "for var i in [P.F(4), P.F(5), P.F(6)] do "
                 "  if i = 50 then continue; end if; u := u + i; "
                 "end for; "
                 "Console.WriteLine($\"t={t} u={u}\");",
                 "public static method F(x: int): int "
                 "  Console.WriteLine($\"F{x}\"); return x * 10; end F;")),
        std::string("F1\nF2\nF3\nF4\nF5\nF6\nt=10 u=100"));
}

// ===========================================================================
// Labeled forms and nesting.
// ===========================================================================

// §21.5 (#6139): `break lp;` / `continue lq;` naming the literal loop itself.
// Before: t=33.
BEZEL_TEST(for_inline_literal_break_continue_6139,
           labeled_break_and_continue_of_the_literal_loop_spec_21_5) {
    BEZEL_EXPECT_EQ(
        out(prog("var t := 0; "
                 "lp: for var i in [0, 1, 2] do "
                 "  if i = 1 then break lp; end if; t := t + 1; end lp; "
                 "lq: for var i in [0, 1, 2] do "
                 "  if i = 1 then continue lq; end if; t := t + 10; end lq; "
                 "Console.WriteLine($\"t={t}\");")),
        std::string("t=21"));
}

// §21.5 (#6139): a literal loop nested in a literal loop — the unlabeled forms
// target the inner one, the labeled forms the outer one. i=0 adds 10 + 30 + 1;
// i=1 continues the outer loop at once; i=2 adds 10, then breaks out at j=30.
// Before: t=183.
BEZEL_TEST(for_inline_literal_break_continue_6139,
           literal_loop_nested_in_literal_loop_spec_21_5) {
    BEZEL_EXPECT_EQ(
        out(prog("var t := 0; "
                 "o: for var i in [0, 1, 2] do "
                 "  for var j in [10, 20, 30] do "
                 "    if j = 20 then continue; end if; "
                 "    if i = 1 then continue o; end if; "
                 "    if i = 2 and j = 30 then break o; end if; "
                 "    t := t + j; "
                 "  end for; "
                 "  t := t + 1; "
                 "end o; "
                 "Console.WriteLine($\"t={t}\");")),
        std::string("t=51"));
}

// §21.5 (#6139): an OUTER literal loop targeted by label from an inner
// named-array loop. Before: t=3032 (the labeled jumps resolved against a stack
// that was missing the literal loop's frame).
BEZEL_TEST(for_inline_literal_break_continue_6139,
           outer_literal_loop_targeted_from_inner_named_loop_spec_21_5) {
    BEZEL_EXPECT_EQ(
        out(prog("using var xs: Array<int> := [7, 8, 9]; var t := 0; "
                 "o: for var i in [0, 1, 2] do "
                 "  for var x in xs do "
                 "    if x = 8 then continue o; end if; "
                 "    if i = 2 then break o; end if; "
                 "    t := t + x; "
                 "  end for; "
                 "  t := t + 1000; "
                 "end o; "
                 "Console.WriteLine($\"t={t}\");")),
        std::string("t=14"));
}

// ===========================================================================
// Jumps through intervening scopes (§21.5: "each performs its ordinary exit
// obligations").
// ===========================================================================

// §21.5 / §31.2 (#6139): `continue` and `break` out of a `try` body run the
// pending `finally` on the way. Before: "t=44 f=4".
BEZEL_TEST(for_inline_literal_break_continue_6139,
           jump_out_of_try_runs_finally_spec_21_5_31_2) {
    BEZEL_EXPECT_EQ(
        out(prog("var t := 0; var f := 0; "
                 "for var i in [0, 1, 2, 3] do "
                 "  try "
                 "    if i = 1 then continue; end if; "
                 "    if i = 2 then break; end if; "
                 "    t := t + 1; "
                 "  finally "
                 "    f := f + 1; "
                 "  end try; "
                 "  t := t + 10; "
                 "end for; "
                 "Console.WriteLine($\"t={t} f={f}\");")),
        std::string("t=11 f=3"));
}

// §21.5 / §28.10 (#6139): a body `using var` is disposed on the jump edge,
// exactly once, across 100 rounds. Before the fix the `continue` found the
// WHILE's frame and skipped its `n := n + 1` — the program never terminated.
BEZEL_TEST(for_inline_literal_break_continue_6139,
           jump_disposes_body_using_var_once_spec_21_5_28_10) {
    BEZEL_EXPECT_EQ(
        out(prog("var t := 0; var n := 0; "
                 "while n < 100 do "
                 "  for var i in [0, 1, 2, 3] do "
                 "    using var d := new D(i); "
                 "    if i = 1 then continue; end if; "
                 "    if i = 2 then break; end if; "
                 "    t := t + 1; "
                 "  end for; "
                 "  n := n + 1; "
                 "end while; "
                 "Console.WriteLine($\"t={t} live={D.Live}\");",
                 "", kLive)),
        std::string("t=100 live=0"));
}

// §21.5 (#6139): jumps from inside a `case` arm and a `do … end` block.
// Before: "42 3".
BEZEL_TEST(for_inline_literal_break_continue_6139,
           jump_from_case_arm_and_do_block_spec_21_5) {
    BEZEL_EXPECT_EQ(
        out(prog("var t := 0; var u := 0; "
                 "for var i in [0, 1, 2, 3] do "
                 "  case i of "
                 "    1 => do continue; end, "
                 "    3 => do break; end, "
                 "    _ => t := t + 1 "
                 "  end case; "
                 "  t := t + 10; "
                 "end for; "
                 "for var i in [0, 1, 2] do "
                 "  do if i = 1 then break; end if; end do; u := u + 1; "
                 "end for; "
                 "Console.WriteLine($\"{t} {u}\");")),
        std::string("22 1"));
}

// ===========================================================================
// Contexts.
// ===========================================================================

// §21.5 (#6139): the literal loop in a lambda body and in a static constructor.
// Before: "4 4".
BEZEL_TEST(for_inline_literal_break_continue_6139,
           lambda_body_and_static_constructor_spec_21_5) {
    BEZEL_EXPECT_EQ(
        out(prog("var f: () -> int := lambda() => do "
                 "  var t := 0; "
                 "  for var i in [0, 1, 2, 3] do "
                 "    if i = 1 then continue; end if; "
                 "    if i = 3 then break; end if; t := t + 1; "
                 "  end for; "
                 "  return t; "
                 "end; "
                 "Console.WriteLine($\"{f()} {P.T}\");",
                 "public static T: int := 0; "
                 "static constructor() "
                 "  for var i in [0, 1, 2, 3] do "
                 "    if i = 1 then continue; end if; "
                 "    if i = 3 then break; end if; P.T := P.T + 1; "
                 "  end for; "
                 "end constructor;")),
        std::string("2 2"));
}

// §21.5 / §21.4 (#6139): the loop variable captured by a lambda in the body —
// "freshly bound on each iteration" — with a `continue` and a `break` after the
// capture. Before: t=100.
BEZEL_TEST(for_inline_literal_break_continue_6139,
           loop_variable_captured_by_lambda_spec_21_5_21_4) {
    BEZEL_EXPECT_EQ(
        out(prog("var t := 0; "
                 "for var i in [1, 2, 3, 4] do "
                 "  var f: () -> int := lambda() => i * 10; "
                 "  if i = 2 then continue; end if; "
                 "  if i = 4 then break; end if; "
                 "  t := t + f(); "
                 "end for; "
                 "Console.WriteLine($\"t={t}\");")),
        std::string("t=40"));
}

// ===========================================================================
// CONTROLS — already right before the fix; they must stay right.
// ===========================================================================

// CONTROL §21.5: the same loops over a NAMED Array<int>, a named inline array,
// a range and a `while`.
BEZEL_TEST(for_inline_literal_break_continue_6139,
           control_named_array_inline_array_range_while_spec_21_5) {
    BEZEL_EXPECT_EQ(
        out(prog("using var xs: Array<int> := [0, 1, 2, 3]; "
                 "var a: [4]int := [0, 1, 2, 3]; "
                 "var t := 0; var u := 0; var v := 0; var w := 0; "
                 "for var i in xs do "
                 "  if i = 1 then continue; end if; "
                 "  if i = 3 then break; end if; t := t + 1; end for; "
                 "for var i in a do "
                 "  if i = 1 then continue; end if; "
                 "  if i = 3 then break; end if; u := u + 1; end for; "
                 "for var i in 0..3 do "
                 "  if i = 1 then continue; end if; "
                 "  if i = 3 then break; end if; v := v + 1; end for; "
                 "var k := 0; "
                 "while k < 4 do "
                 "  k := k + 1; "
                 "  if k = 2 then continue; end if; "
                 "  if k = 4 then break; end if; w := w + 1; end while; "
                 "Console.WriteLine($\"{t} {u} {v} {w}\");")),
        std::string("2 2 2 2"));
}

// CONTROL §21.5: a literal loop with NO jump in its body, and one whose only
// jumps belong to a `while` nested inside it (that inner loop always had its
// own frame).
BEZEL_TEST(for_inline_literal_break_continue_6139,
           control_literal_loop_without_own_jump_spec_21_5) {
    BEZEL_EXPECT_EQ(
        out(prog("var t := 0; var u := 0; "
                 "for var i in [1, 2, 3] do t := t + i; end for; "
                 "for var i in [0, 1, 2] do "
                 "  var k := 0; "
                 "  while true do "
                 "    k := k + 1; "
                 "    if k = 3 then break; end if; "
                 "    if k = 1 then continue; end if; "
                 "    u := u + 1; "
                 "  end while; "
                 "  u := u + 10; "
                 "end for; "
                 "Console.WriteLine($\"{t} {u}\");")),
        std::string("6 33"));
}

// CONTROL §21.7 / §21.8: `return` and `throw` from inside a literal loop never
// went through the loop frame.
BEZEL_TEST(for_inline_literal_break_continue_6139,
           control_return_and_throw_from_literal_loop_spec_21_7_21_8) {
    BEZEL_EXPECT_EQ(
        out(prog("var t := 0; "
                 "try "
                 "  for var i in [0, 1, 2] do "
                 "    if i = 1 then throw new InvalidOperationException(\"x\"); "
                 "    end if; "
                 "    t := t + 1; "
                 "  end for; "
                 "catch "
                 "  InvalidOperationException => do t := t + 100; end "
                 "end try; "
                 "Console.WriteLine($\"{P.F()} {t}\");",
                 "public static method F(): int "
                 "  var t := 0; "
                 "  for var i in [0, 1, 2] do "
                 "    if i = 1 then return t + 50; end if; t := t + 1; "
                 "  end for; "
                 "  return t; "
                 "end F;")),
        std::string("51 101"));
}
