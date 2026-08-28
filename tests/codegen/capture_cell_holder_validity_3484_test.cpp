// Codegen — issue #3484 (§27.5 Closures, spec v0.283): a lambda-captured
// variable's CELL HOLDER is null on any path that did not reach the capture
// site, and every later read of the variable dereferenced it.
//
// §27.5 makes a capture a heap cell shared by the frame and every closure over
// it. `boxLocalSlot` implements that by rebinding the variable's slot to a
// ptr-typed HOLDER alloca and allocating the cell behind a `cell.isnull` branch
// emitted AT THE CAPTURE SITE. Every read and write of the variable after that
// point loads the holder and dereferences it, so the whole scheme rests on one
// obligation: the holder must be valid on every path that reaches such a read.
//
//     method Step(v: int): int
//       var s := 0;
//       if v mod 2 = 0 then
//         var g := lambda(): int => v;    /* allocates the cell HERE only */
//         s := g();
//       end if;
//       return s + v;                     /* odd v: loads the entry null */
//
//     AddressSanitizer: SEGV on unknown address 0x000000000000
//
// Purely synchronous: no `await`, no generic, no unsafe construct. Measured on
// six spellings below — a `case` arm with and without a `do … end`, an `if`
// body, an if-EXPRESSION arm at root depth, and a `while` the compiler cannot
// prove runs at least once — over a captured PARAMETER and a captured LOCAL, and
// with the surviving reader being either an ordinary read or a `using var`'s own
// scope-end DISPOSAL (which goes through the same funnel).
//
// WHICH HALF IS WRONG is decided by the design of the holder, not by taste. The
// guarded strategy is what MAKES the holder nullable — that null is how the
// runtime tells a first pass from a later one across a loop back-edge — so the
// holder is correct and the READS are the wrong half. The fix repairs them at
// `boxedStorage`, the single funnel every boxed read and write already goes
// through, by falling back to the PRE-BOXING alloca recorded in
// `cellVarSyncSlot_` when the holder is null. That is not an approximation: a
// null holder means no closure exists yet, so nothing else can observe the
// variable and the pre-boxing alloca IS its storage — and it is also exactly
// the slot `boxLocalSlot` reads to seed the cell if the capture site is reached
// later, so a write parked there is picked up rather than lost.
//
// ONLY THE EXPOSED READS ARE PATCHED. A read the capture site's `cell.done` join
// DOMINATES can never see the null, and those are left bit-identical. That is
// not an optimization: an unconditional guard perturbs register allocation
// enough to change which stale stack slots still hold a heap pointer when
// LeakSanitizer scans at exit, and five one-shot leak rows elsewhere were
// passing on exactly that accident. Since dominance is a question about the
// FINISHED CFG, `boxedStorage` records the read and `fixupCapturedCellHolderReads`
// patches from `run()`.
//
// DIRECTION OF DANGER. Read-side only. It widens no free, shortens no lifetime
// and adds no release: a null dereference becomes a load/store of storage that
// lives as long as the frame, and a cell that is never allocated is still never
// freed (`free(null)` was already a no-op), so nothing here can double-free.
//
// WHAT IS NOT FIXED HERE. Every row below still LEAKS its capture cell, so these
// are `runAot` VALUE rows, not `runAotChecked` leak rows. That leak is the
// separate reclaim question tracked by #3403 / #3435 / #3426 / #3455 / #3459 —
// a body whose capture site sits behind a block split always takes
// `boxLocalSlot`'s GUARDED path, which `lowerLambda`'s scope-end admission
// excludes. Asserting leak-freedom here would enshrine another issue's fix as
// this one's contract. The cross-SUSPENSION twin of the null holder is #3466,
// covered by its own file.
//
// FAIL-WITHOUT / PASS-WITH, measured program-by-program on one build of each
// compiler (`bezelc --sanitize=address`):
//
//   row                                          pre-fix          post-fix
//   case arm, do-block capture, read after        SEGV @ 0         67200
//   case arm, no do-block, read after             SEGV @ 0         67200
//   if body capture, read after                   SEGV @ 0         67200
//   if-EXPRESSION arms at root depth              SEGV @ 0         30000
//   captured LOCAL behind an `if`, read after     SEGV @ 0         134400
//   zero-trip loop, read after                    SEGV @ 0         30
//   zero-trip loop, disposal is the only reader   SEGV @ 0         400
//   control: same program with NO lambda          67200            67200
//   control: capture behind an `if`, no read      22350            22350
//
// per §27.5 (issue #3484)

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
// These shapes leak their capture cell by a separate, still-open defect
// (#3403 / #3435 family), so LeakSanitizer is not the oracle here. The oracle
// is the exact VALUE printed: a null-holder read cannot produce it, because the
// program dies at address 0 before reaching the print.
std::string out(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// #3484 — the holder is null on the path that did not reach the capture site
// ═══════════════════════════════════════════════════════════════════════════

// A `case` arm taken on half the calls boxes `v`; the other half never runs the
// arm, so `return s + v` loads the entry block's null. 300 calls, so the odd
// path is exercised 150 times. Step(v) = (v even ? v : 0) + v, summed over
// 0..299 = 67200 — the SAME total the no-lambda control below prints, which is
// what makes it the right number rather than merely a number.
// per §27.5 (issue #3484)
BEZEL_TEST(capture_cell_holder_validity_3484,
           case_arm_capture_then_read_param_spec_27_5) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static method Step(v: int): int "
        "    var s := 0; "
        "    case v mod 2 of "
        "      0 => do var g := lambda(): int => v; s := g(); end, "
        "      _ => s := 0 "
        "    end case; "
        "    return s + v; "
        "  end Step; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<300 do acc := acc + C.Step(j); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "67200\n");
}

// The same shape with the arm's `do … end` removed and the lambda invoked
// immediately. `do … end` emits no basic block of its own, so this row proves
// the split that routes the capture onto the guarded path is the CASE ARM, not
// the scoped block inside it.
// per §27.5 (issue #3484)
BEZEL_TEST(capture_cell_holder_validity_3484,
           case_arm_iife_capture_then_read_param_spec_27_5) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static method Step(v: int): int "
        "    var s := 0; "
        "    case v mod 2 of "
        "      0 => s := (lambda(): int => v)(), "
        "      _ => s := 0 "
        "    end case; "
        "    return s + v; "
        "  end Step; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<300 do acc := acc + C.Step(j); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "67200\n");
}

// An ordinary `if` whose condition is false half the time — no `case`, no
// pattern matching. Same total, so the three spellings agree on the value as
// well as on not crashing.
// per §27.5 (issue #3484)
BEZEL_TEST(capture_cell_holder_validity_3484,
           if_body_capture_then_read_param_spec_27_5) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static method Step(v: int): int "
        "    var s := 0; "
        "    if v mod 2 = 0 then "
        "      var g := lambda(): int => v; "
        "      s := g(); "
        "    end if; "
        "    return s + v; "
        "  end Step; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<300 do acc := acc + C.Step(j); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "67200\n");
}

// An if-EXPRESSION at root depth: BOTH arms build a closure, so the parameter is
// captured on every path — and it still crashed, because the two arms are still
// two basic blocks and only one of them holds a `cell.alloc` for `v`.
// Step(v) = (v even ? v + 1 : 1) + v over 0..199 = 19900 + 10100 = 30000.
// per §27.5 (issue #3484)
BEZEL_TEST(capture_cell_holder_validity_3484,
           if_expression_arm_capture_then_read_param_spec_27_5) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static method Step(v: int): int "
        "    var t := 0; "
        "    if v >= 0 then t := 1; end if; "
        "    var f := (if v mod 2 = 0 then lambda(): int => v + t "
        "              else lambda(): int => t); "
        "    return f() + v; "
        "  end Step; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<200 do acc := acc + C.Step(j); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "30000\n");
}

// The same defect over a captured LOCAL rather than a parameter. `boxedStorage`
// is the funnel for both storage classes, so one repair covers both — and this
// row is what says so rather than leaving the local half untested.
// Step(n) = (n even ? 2n : 0) + 2n over 0..299 = 89400 + 45000 = 134400.
// per §27.5 (issue #3484)
BEZEL_TEST(capture_cell_holder_validity_3484,
           if_body_capture_then_read_local_spec_27_5) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static method Step(n: int): int "
        "    var k := n * 2; "
        "    var s := 0; "
        "    if n mod 2 = 0 then "
        "      var g := lambda(): int => k; "
        "      s := g(); "
        "    end if; "
        "    return s + k; "
        "  end Step; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<300 do acc := acc + C.Step(j); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "134400\n");
}

// CONTROL — the identical program with the lambda deleted. It passes on the
// pre-fix compiler too; that is the point. It fixes the expected total as an
// independently-derived number and shows the crash needs the CAPTURE, not the
// `case`.
// per §27.5 (issue #3484)
BEZEL_TEST(capture_cell_holder_validity_3484,
           control_case_arm_without_lambda_is_clean_spec_27_5) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static method Step(v: int): int "
        "    var s := 0; "
        "    case v mod 2 of "
        "      0 => s := v, "
        "      _ => s := 0 "
        "    end case; "
        "    return s + v; "
        "  end Step; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<300 do acc := acc + C.Step(j); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "67200\n");
}

// CONTROL — the conditional capture WITHOUT the later read. Also green pre-fix.
// It is the discriminator that separates this defect from the ordinary
// capture-cell leak: the same program leaks identically on both sides, and only
// the LATER READ of the parameter turns that leak into a crash.
// Step(v) = (v even ? v : 0) over 0..299 = 22350.
// per §27.5 (issue #3484)
BEZEL_TEST(capture_cell_holder_validity_3484,
           control_conditional_capture_without_later_read_spec_27_5) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static method Step(v: int): int "
        "    var s := 0; "
        "    case v mod 2 of "
        "      0 => s := (lambda(): int => v)(), "
        "      _ => s := 0 "
        "    end case; "
        "    return s; "
        "  end Step; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<300 do acc := acc + C.Step(j); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "22350\n");
}


// ── The LOOP spelling: a `while` the compiler cannot prove runs at least once.
// The capture site is in the loop body, so the cell is allocated only if the
// body runs, while `return box.V` after the loop reads it through the holder
// either way. Step(0) took that read straight to address 0 on the pre-fix
// compiler; Step(3) exercises the ordinary path, so the printed total pins BOTH.
// 0 + 3*10 = 30.
//
// This is the shape that already existed, unnoticed, in
// closure_literal_arg_leak_1073_test.cpp's in-loop row — see the oracle note
// this fix added there.
// per §27.5 (issue #3484)
BEZEL_TEST(capture_cell_holder_validity_3484,
           zero_trip_loop_capture_then_read_local_spec_27_5) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class Box "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box; "
        "class Sink "
        "  public method Take(h: () -> ()) h(); end Take; "
        "end Sink; "
        "class C "
        "  public static method Step(n: int): int "
        "    using var box := new Box(); "
        "    using var s := new Sink(); "
        "    var i := 0; "
        "    while i < n do "
        "      s.Take(lambda () => do box.Add(1); end); "
        "      i := i + 1; "
        "    end; "
        "    return box.V; "
        "  end Step; "
        "  public static method Main(): int "
        "    Console.WriteLine((C.Step(0) + C.Step(3) * 10).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "30\n");
}

// The same zero-trip loop with NO read of the captured variable after it — the
// only surviving reader is the `using var`'s own scope-end DISPOSAL, which also
// goes through boxedStorage and so also loaded the unset holder. Pre-fix this
// died at address 0 without the program ever mentioning `box` after the loop;
// post-fix the Box is disposed through its pre-boxing slot. 0 + 4*100 = 400.
// per §27.5, §28.9 (issue #3484)
BEZEL_TEST(capture_cell_holder_validity_3484,
           zero_trip_loop_capture_disposal_only_reader_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class Box "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box; "
        "class Sink "
        "  public method Take(h: () -> ()) h(); end Take; "
        "end Sink; "
        "class C "
        "  public static method Step(n: int): int "
        "    var seen := 0; "
        "    using var box := new Box(); "
        "    using var s := new Sink(); "
        "    var i := 0; "
        "    while i < n do "
        "      s.Take(lambda () => do box.Add(1); end); "
        "      seen := seen + 1; "
        "      i := i + 1; "
        "    end; "
        "    return seen; "
        "  end Step; "
        "  public static method Main(): int "
        "    Console.WriteLine((C.Step(0) + C.Step(4) * 100).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "400\n");
}
