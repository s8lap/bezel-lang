// Codegen — issue #3455 (§27.5 Closures, §28.9 Runtime-managed types, spec
// v0.283): a lambda's SECOND boxed capture is pushed onto boxLocalSlot's
// GUARDED path by the FIRST one's null-guard, and #694's envdtor capture-cell
// free declined every guarded cell outright — so a local captured by exactly
// one, definitely-escaping lambda (precisely the shape #694 exists to handle)
// was reclaimed by nobody.
//
//     public static method Make(k: int): () -> int
//       var g := lambda() => k;        /* k boxed here, FRESH path        */
//       var t: int := g();
//       var f := lambda() => k + t;    /* k re-boxed: the #2303 null-guard
//                                         splits the block, so t is GUARDED */
//       return f;                      /* 4 B leaked per call, pre-fix     */
//     end Make;
//
// §27.5 makes the cell the implementation's own allocation ("Captured variables
// are heap-allocated in a closure object") and §28.9 makes reclaiming it the
// implementation's own obligation ("Function-typed fields, parameters, and
// locals are not subject to `owned` annotations and carry no disposal
// obligation") — the program has no way to free it, so the compiler owes it.
//
// ── WHY THE OLD SPELLING DECLINED. `wasGuardedLocal` reports boxLocalSlot's
// STRATEGY, which is chosen by "is there a StoreInst to this slot in the CURRENT
// basic block?" — a question about where the IR builder happens to be, not about
// the capture being classified. `lowerLambda` boxes captured PARAMETERS before
// captured LOCALS, and a parameter that arrives on the ALREADY-BOXED path emits
// the #2303 null-guard, whose CondBr moves the insert point into `cell.done`.
// Every later capture in the same lambda therefore reports GUARDED, whatever its
// own shape. Because captured locals are enumerated from an `unordered_map`,
// WHICH capture arrives second is iteration order, so the defect was also
// order-dependent for the all-locals spelling.
//
// ── THE FIX is one disjunct: `loopStack_.empty()` joins the exclusion, making
// this the THIRD and last of the four capture-cell reclaim arms in `lowerLambda`
// to state the SAME proof. The captured-PARAM twin of this branch already read
// `(!wasGuardedParam || loopStack_.empty())` (#3403) and the captured-LOCAL
// scope-end branch already read `(!wasGuardedLocal || isPatternBinding ||
// loopStack_.empty())` (#3435); only this one still spelled it as a flat
// `!wasGuardedLocal`. The proof is about a BACKWARD EDGE, not a storage class:
// what makes a guarded cell unsafe to free from ONE envdtor is that a single
// syntactic lambda node inside a loop builds N closures over ONE cell, so N
// envdtors would free it N times. With no enclosing loop the null-guarded
// allocation runs at most once per invocation, and `singleCaptureLocals_` then
// means exactly one closure holds it.
//
// ── THE DIRECTION OF DANGER. This arm ARMS A FREE: under-admitting leaks,
// over-admitting is a use-after-free or a double-free through an escaped
// closure. Two OVER-RELEASE GUARD rows below assert shapes the widened arm must
// still decline, and two value rows assert that the freed cell is still live for
// every read and write the frame makes through it (§27.5's "Modifications to a
// captured variable inside the lambda are visible outside, and vice versa").
//
// FAIL-WITHOUT / PASS-WITH, measured program-by-program with
// `bezelc --sanitize=address` (LeakSanitizer on), on one build of each compiler:
//
//   program                                       pre-fix (ef339fa)   post-fix
//   #3455 repro, int capture, x500                2000 B / 500        clean
//   #3455 string capture, TWO captures, x500      4000 B / 500        clean
//   #3455 `this.F :=` field-store escape, x500    2000 B / 500        clean
//   value: write through the cell after capture   2000 B / 500        clean
//   GUARD: field-store escape INSIDE a loop        800 B / 200         800 B / 200
//   GUARD: two escaping capturers                26000 B / 1500      26000 B / 1500
//
// The two guard rows leak IDENTICALLY on both sides by design: that equality is
// the assertion that the widened arm admitted nothing extra. Both are ALSO value
// rows, which is what makes them able to catch the over-release direction — an
// arm that fired on them would double-free (the in-loop row) or hand back a
// number read out of freed memory.
//
// ⚠ THE LEAK ROWS NEED THE ASan/LSan ORACLE SPECIFICALLY (`runAotChecked`, not
// `runAot`): the effect under test is a 4-byte allocation nobody frees, which a
// plain build reports as a perfectly correct answer. Do not "simplify" a
// checkedOut row here to plainOut — that silently disarms it. Conversely the two
// GUARD rows are deliberately `runAot` VALUE rows, because their shapes leak by
// design and an ASan/LSan row would enshrine that leak as a contract.
// per §27.5, §28.9 (issue #3455)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// runAotChecked runs the program under generated-code ASan with LeakSanitizer
// on, so a non-empty leak report fails the row before the value is compared.
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
// Plain runAot for the guard rows, whose shapes leak by design (a cell no
// mechanism may reclaim). The oracle there is the exact VALUE: an over-eager
// free shows up as a wrong number, a crash, or an ASan abort under `--full`.
std::string plainOut(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ── The issue's exact repro, LOOP-AMPLIFIED to 500 calls. A one-shot 4-byte
// LSan row can pass on BOTH sides of this fix because the dead frame's copy of
// the cell pointer is still reachable to LSan's conservative root scan; 500
// executions make the pre-fix report 2000 B / 500 allocations, which no
// reachability accident explains. Printed total: 500 x (3 + 3) = 3000.
// per §27.5, §28.9 (issue #3455)
BEZEL_TEST(capture_cell_envdtor_guarded_3455,
           second_boxed_capture_cell_freed_in_envdtor_amplified_500_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method Make(k: int): () -> int "
        "    var g := lambda() => k; "
        "    var t: int := g(); "
        "    var f := lambda() => k + t; "
        "    return f; "
        "  end Make; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..499 do "
        "      var h := C.Make(3); "
        "      acc := acc + h(); "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "3000\n");
}

// ── The same shape over a `string` capture — an 8-byte cell rather than a
// 4-byte one, and a capture whose envdtor ALSO releases the cell's CONTENTS, so
// a fix that armed the free without keeping the content release ordered before
// it would surface as an over-release (a heap-use-after-free on the string) that
// ASan reports as an ERROR rather than as a leak.
//
// ⚠ THE SECOND LAMBDA MUST CAPTURE **BOTH** `k` AND `t`. An earlier revision of
// this row wrote `var f := lambda() => t;` — a single capture — and it was
// measured GREEN ON BOTH COMPILERS, i.e. vacuous as a regression row. That is
// the whole mechanism of this issue restated as a test-design rule: with `f`
// capturing only `t`, nothing is boxed before `t`, `t`'s own store sits in the
// capture site's basic block, boxLocalSlot takes the FRESH path, and the old arm
// already armed. It is `k` — a captured PARAMETER reaching the ALREADY-BOXED
// path, whose #2303 null-guard splits the block — that pushes `t` onto the
// guarded path. Do not "simplify" this row back to one capture.
//
// Pre-fix 4000 B / 500 allocations in the two-capture form; the one-capture form
// was 0 on both sides. Printed total: 500 x Length("ab" + "ab!") = 500 x 5 = 2500.
// per §27.5, §28.9 (issue #3455)
BEZEL_TEST(capture_cell_envdtor_guarded_3455,
           second_boxed_string_capture_cell_freed_amplified_500_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method Make(k: string): () -> string "
        "    var g := lambda() => k; "
        "    var t: string := g() + \"!\"; "
        "    var f := lambda() => k + t; "
        "    return f; "
        "  end Make; "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var j in 0..<500 do "
        "      var h := C.Make(\"ab\"); "
        "      n := n + h().Length; "
        "    end; "
        "    Console.WriteLine(n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "2500\n");
}

// ── THE ESCAPE ROUTE IS NOT ONLY `return f`. `definitelyEscapingLambdas_` also
// admits a `this.F := lambda …` assigned EXACTLY ONCE in the method (the closure
// leaves through the receiver), and there the guarded path is reached by an
// ordinary `if` rather than by a preceding capture — which is what makes this
// row a second, independent witness that the trigger is the BLOCK SPLIT and not
// the two-capture arrangement of the repro above. Pre-fix: 2000 B / 500.
// Printed total: 500 x 2.
// per §27.5, §28.9 (issue #3455)
BEZEL_TEST(capture_cell_envdtor_guarded_3455,
           this_field_store_escape_guarded_capture_freed_amplified_500_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class H "
        "  public F: () -> int := lambda(): int => 0; "
        "  public method Setup(n: int) "
        "    var t := 0; "
        "    if n > 0 then t := n; end if; "
        "    this.F := lambda(): int => t; "
        "  end Setup; "
        "end H; "
        "class C "
        "  public static method Run(n: int): int "
        "    using var h := new H(); "
        "    h.Setup(n); "
        "    return h.F(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<500 do acc := acc + C.Run(2); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "1000\n");
}

// ── VALUE + LEAK row: the frame WRITES the captured local after the capture and
// the escaped closure must observe the write (§27.5: "Modifications to a
// captured variable inside the lambda are visible outside, and vice versa").
// This is the row that would catch a fix that freed the cell too early — the
// write `t := t + 10` goes THROUGH the cell holder, and the returned closure
// reads it after the frame is gone. Pre-fix this leaked 2000 B / 500 as well.
// Printed total: 500 x (3 + (3 + 10)) = 8000.
// per §27.5, §28.9 (issue #3455)
BEZEL_TEST(capture_cell_envdtor_guarded_3455,
           write_through_cell_after_capture_still_visible_amplified_500_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method Make(k: int): () -> int "
        "    var g := lambda() => k; "
        "    var t: int := g(); "
        "    var f := lambda() => k + t; "
        "    t := t + 10; "
        "    return f; "
        "  end Make; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<500 do "
        "      var h := C.Make(3); "
        "      acc := acc + h(); "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "8000\n");
}

// ── OVER-RELEASE GUARD: the `loopStack_.empty()` conjunct itself. One syntactic
// `this.F := lambda` inside a `while` builds a NEW closure every iteration over
// the ONE guarded cell, and each of those closures is released as the field is
// overwritten — so an arm that armed here would run the cell free once per
// iteration: a double-free, not a leak. The cell is deliberately left to leak
// (800 B / 200 calls = 4 B per call, IDENTICAL before and after the fix); the
// oracle is the VALUE, which an over-eager free turns into an ASan abort or a
// number read out of freed memory. Printed total: 200 x 2.
// per §27.5, §28.9 (issue #3455)
BEZEL_TEST(capture_cell_envdtor_guarded_3455,
           guard_field_store_escape_inside_loop_not_freed_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(plainOut(
        "module M; import System; "
        "class H "
        "  public F: () -> int := lambda(): int => 0; "
        "  public method Setup(n: int) "
        "    var t := 0; "
        "    if n > 0 then t := n; end if; "
        "    var i := 0; "
        "    while i < 3 do "
        "      this.F := lambda(): int => t; "
        "      i := i + 1; "
        "    end; "
        "  end Setup; "
        "end H; "
        "class C "
        "  public static method Run(n: int): int "
        "    using var h := new H(); "
        "    h.Setup(n); "
        "    return h.F(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<200 do acc := acc + C.Run(2); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "400\n");
}

// ── OVER-RELEASE GUARD: the `singleCaptureLocals_` conjunct. TWO lambda nodes
// capture `t` and either may be the one returned, so no single envdtor is
// provably the cell's last reader and the free must stay declined. Leaks
// identically before and after (26000 B / 1500 allocations over 500 calls — the
// second closure, its env and the two cells); the oracle is the VALUE.
// Printed total: 500 x (3 + 3) = 3000 (`k > 0`, so `f1` is always the one
// returned).
// per §27.5, §28.9 (issue #3455)
BEZEL_TEST(capture_cell_envdtor_guarded_3455,
           guard_two_escaping_capturers_not_freed_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(plainOut(
        "module M; import System; "
        "class C "
        "  public static method Make(k: int): () -> int "
        "    var g := lambda() => k; "
        "    var t: int := g(); "
        "    var f1 := lambda() => k + t; "
        "    var f2 := lambda() => t + 1; "
        "    if k > 0 then return f1; end if; "
        "    return f2; "
        "  end Make; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..499 do "
        "      var h := C.Make(3); "
        "      acc := acc + h(); "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "3000\n");
}
