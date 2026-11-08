// Regression tests for #8328 -- §28.3's STATE MATCH ON REASSIGNMENT did not
// fire INSIDE A LOOP BODY, and the generic `t.Result` still leaked (#8327's
// sibling, see the last row group).
//
// THE DEFECT (MEASURED on the base, 74c9d282, by r40-verify-3 and re-measured
// here). §28.3 (v0.355) *State match on reassignment*: "Assigning a borrow to
// an owned-state LHS, or an owned value to a borrowed-state LHS, is rejected
// -- the mismatch would silently drop or double-hold a disposal obligation",
// and item 1: "an owned-state LHS requires an owned RHS -- a fresh producer,
// or a named owned binding under the explicit transfer marker (`x := move y`)"
// -- "and disposes the displaced value per drop-on-overwrite (§28.7)".
//
//   static method Run(b: Cell): int
//     var x := new Cell(1);          // OWNED
//     while i < 3 do
//       x := b;                     // <-- a BORROW into an OWNED-state LHS
//       i := i + 1;
//     end while;
//
// The STRAIGHT-LINE twin (`x := b;` outside the loop) is REJECTED with §28.3's
// state-match diagnostic; the IN-LOOP form COMPILED and was a heap-use-after-
// free under ASan: the local's §28.7 drop-on-overwrite and its §28.3 scope-end
// auto-drop disposed the CALLER's `b`.
//
// WHERE THE OWNED STATE WAS LOST (the mechanism). §28.3 *Ownership state of
// locals*: "At every control-flow join the compiler applies K7's meet to each
// variable's tracked state". A loop header is such a join: the pre-header
// edge (the local is OWNED) meets the back edge (the local is BORROWED -- the
// violating store's effect). K7.3's mixed merge (owned ⊓ borrowed) is
// REJECTED, and `joinInto` implements that as POISON
// (src/ownership/Ownership_Checks.cpp's `ownedBorrowedClash` arm). The Poison
// then hid the very violation that produced it: the assignment is re-examined
// in the FINAL emit pass against the loop header's CONVERGED state, which is
// no longer Owned, so §28.3's state match -- the rule that rejects the store
// -- never fired.
//
// THE FIX (src/ownership/Ownership.cpp, `MethodOwnership::handleAssignment`).
// A REJECTED store never happens: the binding keeps the value -- and the
// ownership STATE -- it held. The back edge then carries the same OWNED state
// the pre-header edge does, the loop-header join is owned ⊓ owned (no K7.3
// mixed merge left to hide the violation), and the FINAL emit pass rejects the
// store AT THE ASSIGNMENT with the SAME diagnostic the straight-line twin
// draws.
//
// THE ORACLE. Every rejection row asserts the strict oracle
// (`failedAtCompileTimeWithRealLocation`: the driver really rejected, with a
// real `<path>:<line>:<col>:` location -- not a crash, not a toolchain skip)
// plus the rule's own diagnostic text, and that the program did NOT run. The
// admitting rows (the borrow-into-borrow twin, the `move` forms, the producer
// form) are the CONTROLS: they must keep compiling and running, so a fix that
// simply rejected every store in a loop would be caught.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// §28.3's state-match diagnostic, quoted from the compiler's own text. The
// long sentence is split so the row pins the rule, not the phrasing of one
// fragment: the LHS's OWNED state, the RHS's BORROW classification and the
// §28.3 citation are the three things the rule must name.
constexpr const char* kStateMatchOwned =
    "is in the OWNED state and the right-hand side is an unmarked read of";
constexpr const char* kStateMatchRule =
    "§28.3's state match on reassignment requires an owned-state LHS to "
    "receive an owned value";
constexpr const char* kStateMatchBorrow = "which is a BORROW";

// The straight-line twin's diagnostic, byte for byte. The in-loop row asserts
// the SAME text, so a future re-wording of one spelling cannot drift from the
// other without failing this row.
constexpr const char* kStateMatchRemedy =
    "Bind the borrow to a separate local instead";

std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// The program under test. `Cell` is a class with a destructor and a static
// live counter, so "the destructor ran exactly once" is directly observable.
// `b` is a plain (borrowed) parameter: the unmarked read of it is the BORROW
// §28.3's adoption clause names.
std::string prog(const std::string& body) {
    return
        "module M; import System; "
        "class Cell "
        "  public static n: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class C "
        "  static method Run(b: Cell, i: int): int "
        "    var x := new Cell(1); "
        + body +
        "    return 1; "
        "  end Run; "
        "  public static method Main(): int "
        "    Run(new Cell(2), 3); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// The state-match rejection, asserted at the strict oracle. The diagnostic
// must name the LHS's OWNED state, the RHS's BORROW classification and the
// §28.3 rule, and the program must NOT have run.
void expectStateMatchRejected(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find(kStateMatchOwned) != std::string::npos);
    BEZEL_EXPECT_TRUE(text.find(kStateMatchBorrow) != std::string::npos);
    BEZEL_EXPECT_TRUE(text.find(kStateMatchRule) != std::string::npos);
    BEZEL_EXPECT_FALSE(r.ran);
}

}  // namespace

// ROW 1 -- THE DEFECT: the issue's repro verbatim, the assignment INSIDE A
// LOOP BODY. On the base this COMPILES (rc=0) and is a heap-use-after-free
// under ASan; after the fix it is REJECTED at compile time with §28.3's
// state-match diagnostic -- the same diagnostic the straight-line twin draws.
BEZEL_TEST(state_match_in_loop_body_8328,
           owned_lhs_borrow_rhs_inside_while_body_is_rejected_spec_28_3) {
    JitResult r = runAotChecked(prog(
        "    var i := 0; "
        "    while i < 3 do "
        "      x := b; "
        "      i := i + 1; "
        "    end while; "));
    expectStateMatchRejected(r);
    // The remedy the straight-line twin names -- the same text, so the
    // in-loop spelling is not a second, differently-worded rule.
    BEZEL_EXPECT_TRUE(diagnosticText(r).find(kStateMatchRemedy) !=
                      std::string::npos);
}

// ROW 2 -- THE CONTROL: the byte-identical program with the assignment
// OUTSIDE the loop. It is rejected on the base too -- that is the point: it
// is the twin the in-loop form must now match, so the pair differs only in
// the loop-head join and not in any unrelated rule.
BEZEL_TEST(state_match_in_loop_body_8328,
           owned_lhs_borrow_rhs_straight_line_is_rejected_spec_28_3) {
    JitResult r = runAotChecked(prog("    x := b; "));
    expectStateMatchRejected(r);
    BEZEL_EXPECT_TRUE(diagnosticText(r).find(kStateMatchRemedy) !=
                      std::string::npos);
}

// ROW 3 -- the same shape in a `for` body. The loop-head join is the same
// kind of join (a back edge into the header), so the rejection must fire
// there too.
BEZEL_TEST(state_match_in_loop_body_8328,
           owned_lhs_borrow_rhs_inside_for_body_is_rejected_spec_28_3) {
    JitResult r = runAotChecked(prog(
        "    for var k in 0..3 do "
        "      x := b; "
        "    end for; "));
    expectStateMatchRejected(r);
}

// ROW 4 -- the same shape in a NESTED loop body: the inner header's join is
// the inner back edge, and the outer header's join sees the inner body's
// exit. Both must not hide the violation. The loops are BOUNDED, so the
// program runs (rather than spinning) on a compiler that admits it -- the
// row's oracle is the rejection, and a bounded program makes the pre-fix
// failure a plain "not rejected" instead of a harness timeout.
BEZEL_TEST(state_match_in_loop_body_8328,
           owned_lhs_borrow_rhs_inside_nested_loop_body_is_rejected_spec_28_3) {
    JitResult r = runAotChecked(prog(
        "    var i := 0; "
        "    while i < 3 do "
        "      var j := 0; "
        "      while j < 3 do "
        "        x := b; "
        "        j := j + 1; "
        "      end while; "
        "      i := i + 1; "
        "    end while; "));
    expectStateMatchRejected(r);
}

// ROW 5 -- the `OwningTask` spelling of #8137: `var c := t.Result;` is an
// OWNED local (§30.3's transfer), so `c := b;` inside a `while` body is the
// same §28.3 state-match violation. On the base this compiled and was a
// heap-use-after-free under ASan.
BEZEL_TEST(state_match_in_loop_body_8328,
           owning_task_result_owned_lhs_borrow_rhs_in_loop_is_rejected_spec_28_3) {
    JitResult r = runAotChecked(
        "module M; import System; "
        "class Cell "
        "  public static n: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static async method Mk(): OwningTask<Cell> return new Cell(5); end Mk; "
        "  static method Run(b: Cell, i: int): int "
        "    using var t := Mk(); "
        "    var c := t.Result; "
        "    var k := 0; "
        "    while k < 3 do "
        "      c := b; "
        "      k := k + 1; "
        "    end while; "
        "    return 1; "
        "  end Run; "
        "  public static method Main(): int "
        "    Run(new Cell(2), 3); "
        "    return 0; "
        "  end Main; "
        "end C;");
    expectStateMatchRejected(r);
}

// ROW 6 -- the `owned` PARAMETER spelling: an `owned` parameter is an
// OWNED-state binding too (§28.3), so `x := b;` inside a loop body is the
// same violation.
BEZEL_TEST(state_match_in_loop_body_8328,
           owned_param_lhs_borrow_rhs_in_loop_is_rejected_spec_28_3) {
    JitResult r = runAotChecked(
        "module M; import System; "
        "class Cell "
        "  public static n: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class C "
        "  static method Run2(owned x: Cell, b: Cell): int "
        "    var i := 0; "
        "    while i < 3 do "
        "      x := b; "
        "      i := i + 1; "
        "    end while; "
        "    return 1; "
        "  end Run2; "
        "  public static method Main(): int "
        "    Run2(new Cell(1), new Cell(2)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    expectStateMatchRejected(r);
}

// ROW 7 -- THE ADMITTING TWIN (the positive control): a BORROW-state LHS
// receiving a borrow inside a loop body is §28.3 item 1's other half ("a
// borrowed-state LHS requires a borrow (or `null`) RHS") and must STAY
// ADMITTED. This row is green on the base and on the tip; it is what proves
// the fix rejects the OWNED-state case and not "every store in a loop".
BEZEL_TEST(state_match_in_loop_body_8328,
           borrowed_lhs_borrow_rhs_inside_while_body_is_admitted_spec_28_3) {
    JitResult r = runAotChecked(
        "module M; import System; "
        "class Cell "
        "  public static n: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class C "
        "  static method Run(b: Cell, i: int): int "
        "    var x := b; "
        "    var k := 0; "
        "    while k < 3 do "
        "      x := b; "
        "      k := k + 1; "
        "    end while; "
        "    return 1; "
        "  end Run; "
        "  public static method Main(): int "
        "    Run(new Cell(2), 3); "
        "    Console.WriteLine(Cell.n); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ROW 8 -- THE PRODUCER FORM (the third control): `x := new Cell(3);` is
// §28.3 item 1's other admitted RHS for an owned-state LHS ("a fresh
// producer"), and §28.7 disposes the displaced value per drop-on-overwrite.
// It must STAY ADMITTED inside a loop body -- three iterations, three
// drop-on-overwrite discharges, and no leak and no double free under the
// ASan/LSan tier.
BEZEL_TEST(state_match_in_loop_body_8328,
           owned_lhs_producer_rhs_inside_while_body_is_admitted_spec_28_3) {
    JitResult r = runAotChecked(
        "module M; import System; "
        "class Cell "
        "  public static n: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class C "
        "  static method Run(b: Cell, i: int): int "
        "    var x := new Cell(1); "
        "    var k := 0; "
        "    while k < 3 do "
        "      x := new Cell(3); "
        "      k := k + 1; "
        "    end while; "
        "    return 1; "
        "  end Run; "
        "  public static method Main(): int "
        "    Run(new Cell(2), 3); "
        "    Console.WriteLine(Cell.n); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The class's live counter: the method's own Cells are all discharged
    // (the initial one by the first drop-on-overwrite, the two later ones by
    // the scope-end auto-drop), so only the CALLER's `new Cell(2)` is alive.
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
    BEZEL_EXPECT_TRUE(diagnosticText(r).find("ERROR: AddressSanitizer") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(diagnosticText(r).find("ERROR: LeakSanitizer") ==
                      std::string::npos);
}

// ROW 9 -- THE `move` FORM (the fourth control): `x := move d;` is §28.3's
// EXPLICIT TRANSFER MARKER -- an owned RHS for an owned-state LHS -- so
// §28.3's state match does NOT apply to it. Inside a LOOP BODY the store is
// nevertheless rejected, by a DIFFERENT rule: a `move` in a loop body is a
// conditional consume, so K7.4's maybe-consumed join rejects it
// ("cannot consume `d`: it may have already been consumed on another branch").
// The row pins that verdict AND that the state-match diagnostic does NOT
// appear -- the fix must not capture the marked-transfer form.
BEZEL_TEST(state_match_in_loop_body_8328,
           owned_lhs_move_rhs_inside_while_body_keeps_its_own_diagnostic_spec_28_3) {
    JitResult r = runAotChecked(
        "module M; import System; "
        "class Cell "
        "  public static n: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class C "
        "  static method Run(b: Cell, i: int): int "
        "    var x := new Cell(1); "
        "    var d := new Cell(3); "
        "    var k := 0; "
        "    while k < 1 do "
        "      x := move d; "
        "      k := k + 1; "
        "    end while; "
        "    return 1; "
        "  end Run; "
        "  public static method Main(): int "
        "    Run(new Cell(2), 3); "
        "    Console.WriteLine(Cell.n); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("maybe-consumed join") != std::string::npos);
    // The state-match diagnostic must NOT be what rejects it.
    BEZEL_EXPECT_TRUE(text.find(kStateMatchRule) == std::string::npos);
    BEZEL_EXPECT_FALSE(r.ran);
}

// ROW 10 -- the same marked transfer OUTSIDE a loop, where the consume is
// unconditional: admitted, and the displaced value is disposed per §28.7's
// drop-on-overwrite. This is the positive control for ROW 9's transfer form.
BEZEL_TEST(state_match_in_loop_body_8328,
           owned_lhs_move_rhs_straight_line_is_admitted_spec_28_3) {
    JitResult r = runAotChecked(
        "module M; import System; "
        "class Cell "
        "  public static n: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class C "
        "  static method Run(b: Cell, i: int): int "
        "    var x := new Cell(1); "
        "    var d := new Cell(3); "
        "    x := move d; "
        "    return 1; "
        "  end Run; "
        "  public static method Main(): int "
        "    Run(new Cell(2), 3); "
        "    Console.WriteLine(Cell.n); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The class's live counter: the method's own Cells are all discharged
    // (the displaced `new Cell(1)` by the drop-on-overwrite, the moved-in
    // `new Cell(3)` by the scope-end auto-drop), so only the CALLER's
    // `new Cell(2)` is alive.
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
    BEZEL_EXPECT_TRUE(diagnosticText(r).find("ERROR: AddressSanitizer") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(diagnosticText(r).find("ERROR: LeakSanitizer") ==
                      std::string::npos);
}
