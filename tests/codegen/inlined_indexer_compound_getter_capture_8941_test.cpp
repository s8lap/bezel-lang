// Regression tests for #8941: the INLINED expression-bodied INDEXER getter of
// a COMPOUND assignment (`h[i] += 1`) handed a capturing lambda the FIRST
// pass's index parameter, so every pass read `i` as 0.
//
// Spec (v0.359):
//  - §23.4: an indexer's parameters are the accessor's parameters, and every
//    invocation of the accessor binds them afresh.
//  - §6.6: "the statement `x op= e` is equivalent to `x := x op e` with the
//    target evaluated once" — the accessor is invoked ONCE per compound
//    assignment, so the getter body runs once per pass.
//  - §27.5: "each execution of its declaration binds a fresh variable, so a
//    lambda formed after it captures that execution's variable".
//
// `lowerIndexerCompoundCore` binds the indexer parameters for the INLINED
// expression-bodied getter and lowers the getter body BEFORE the #8864 mark
// (`loopBodyDeclaredSlots_.insert(...)`) that the SETTER half applies. The
// getter's lambda therefore captured the guarded single cell (`cell.alloc`
// behind `cell.isnull`), which every later pass reused.
//
// MEASURED on 67bcdf33f (before #9004): the issue's repro printed `200` where
// §23.4 / §27.5 require `1425` (sum over i in 0..49 of (i + 3 + 1)).
// On the current r30-c1 the literal repro is ALREADY correct: #9004 changed
// boxLocalSlot's mode choice (dcc970937, 9b0d4ae0e), so the first two rows
// below are base-green CONTROLS, not fail-without pins. They catch a return of
// the original #8941 symptom (every pass reads i as 0) if the getter arm's
// boxing choice regresses. The fail-without pin for this fix is the third row
// (reviewer_getter_then_setter_capture_200x_spec_23_4_27_5).
//
// Each row loops the compound assignment and asserts the exact stdout.
//
// per §6.6, §23.4, §27.5 (v0.359)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string stdoutOf(const std::string& program) {
    auto r = runAot(program);
    return (r.ran && r.exitCode == 0) ? r.stdoutText
                                      : std::string("<did-not-run>");
}
}  // namespace

// BASE-GREEN CONTROL on r30-c1 (not a fail-without pin): the issue's literal
// repro, an expression-bodied indexer getter whose body forms a capturing
// lambda over the index parameter, read through a COMPOUND assignment 50
// times. #9004 already cures it on the base, so it catches only a regression
// of the original symptom.
BEZEL_TEST(inlined_indexer_compound_getter_capture_8941,
           compound_getter_index_capture_50x_spec_23_4_27_5) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public class H "
        "  public n: int; "
        "  public property [i: int]: int "
        "    get => (var g := lambda(): int => 3; var q := g(); "
        "      var k := lambda(): int => i + q; k()); "
        "    set => n := n + value; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); var i := 0; "
        "    while i < 50 do h[i] += 1; i := i + 1; end; "
        "    Console.Write(h.n); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "1425");
}

// BASE-GREEN CONTROL on r30-c1 (not a fail-without pin): the same shape at 200
// passes, sum over i in 0..199 of (i + 3 + 1) = 19900 + 800. Pre-#9004 trees
// printed 800 (every pass read i as 0); on r30-c1 it is already correct.
BEZEL_TEST(inlined_indexer_compound_getter_capture_8941,
           compound_getter_index_capture_200x_spec_23_4_27_5) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public class H "
        "  public n: int; "
        "  public property [i: int]: int "
        "    get => (var g := lambda(): int => 3; var q := g(); "
        "      var k := lambda(): int => i + q; k()); "
        "    set => n := n + value; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); var i := 0; "
        "    while i < 200 do h[i] += 1; i := i + 1; end; "
        "    Console.Write(h.n); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "20700");
}

// FAIL-WITHOUT PIN for the #8941 getter-arm fix (§23.4, §27.5): the getter is
// lowered first and boxes the shared index slot guarded (no #8864 mark); the
// BLOCK setter's lambda then reuses that cell and reads pass 0's i. Each pass
// n += i + value where value = (i + 3) + 1, so sum over i in 0..199 of
// (2i + 4) = 40600. Red on r30-c1 (prints 20700), green with the fix (40600).
BEZEL_TEST(inlined_indexer_compound_getter_capture_8941,
           reviewer_getter_then_setter_capture_200x_spec_23_4_27_5) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public class H "
        "  public n: int; "
        "  public property [i: int]: int "
        "    get => (var g := lambda(): int => 3; var q := g(); "
        "      var k := lambda(): int => i + q; k()); "
        "    set => do "
        "      var g := lambda(): int => 1; var q := g(); "
        "      var k := lambda(): int => i + value + q - 1; n := n + k(); "
        "    end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); var i := 0; "
        "    while i < 200 do h[i] += 1; i := i + 1; end; "
        "    Console.Write(h.n); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "40600");
}
