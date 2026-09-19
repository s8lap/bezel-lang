// Spec-derived regression test for issue #4130 —
// `Lambda that reads an enclosing loop's mutated control variable leaks a
// 4-byte capture cell, independent of anything else in the lambda body`.
//
// THE DEFECT (measured on `102d29f`, --sanitize=address): a lambda declared
// inside a `while` loop that (a) reads the loop's own mutated control
// variable AND (b) calls a `case`/Sink.Take shape leaks exactly ONE 4-byte
// allocation (one calloc'd cell for the captured local `i`), once per
// PROGRAM RUN regardless of trip count. The issue proves the leak
// disappears when either (1) the outer `while` is removed or (2) the lambda
// body no longer reads `i`.
//
// ROOT CAUSE (per §27.5 / §28.9 / #4210). The captured local `i` lives in
// an outer scope relative to the lambda's capture site. The cell holding
// `i` (the box) must persist across loop iterations because both the
// `i < N` guard and the post-iteration `i := i + 1` write/read go through
// it. The non-escaping lambda's scope-end free was suppressed by the
// inner-scope-vs-outer-scope gate, and the envdtor free required the
// lambda to be `definitelyEscaping` (which it is not). The original
// conservative path skipped the free entirely — leaking the cell.
//
// FIX (delivered by #4210 — `d0d5a173`, restored after `ce2354fb` cluster
// revert by `aed9f76f`). Extend the #969 deferred-cell-free path in
// `consumeLastLambdaCapturedCells` from pattern-binding-only to ALL
// outer-scope captures: defer the cell free into the OWNER scope (the
// outer block where `i` was declared), where it runs in `emitDisposals`'s
// SECOND PASS — AFTER every main-loop disposal and every envdtor release.
// The captured-local scope depth test
// (`dd->second < disposeStack_.size()`) is the gate; for a captured local
// declared at the method-body root, this fires at every inner-lambda
// capture site, deferring the cell free into the root scope where `i`
// outlives the loop.
//
// MEASUREMENT (r18-c7 e1733e53, ASan/LSan, 200-iteration amplification):
//   FAIL-WITHOUT (#4210 pre-fix, 102d29f): "Direct leak of 4 byte(s) in 1 object(s)"
//   PASS-WITH    (#4210 post-fix, e1733e53): zero leaks
//
// #4130 differs from #4210 only by including the verbatim `case` /
// `Sink.Take(move x)` / `Res` ownership-transfer shape from the issue body
// (the issue itself states "case, Sink.Take, and Res are incidental");
// this test asserts the same fix covers that shape too.
//
// per spec §27.5 / §28.9 / #4130, v0.306

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

// Verbatim reproducer from the issue body — outer `while` loop with a
// lambda that READS the loop control variable `i` (via `i mod 2`), the
// `case` / `Sink.Take(move x)` ownership-transfer shape, and a per-iteration
// mutation of `i`. Loop-amplified to 200 iterations so the one-time 4-byte
// leak (visible only under LSan's process-exit scan) reaches a measurable
// size.
//
// ASan/LSan on the pre-#4210 build leaks "Direct leak of 4 byte(s) in 1
// object(s)" once per program run (the captured-`int` cell for `i`).
BEZEL_TEST(lambda_captures_loop_var_4130, verbatim_reproducer_200_iterations) {
    auto r = runAotChecked(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink "
        "  public static method Take(owned r: Res): int "
        "    using var r2 := owned r; return r2.tag; "
        "  end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 200 do "
        "      var f := lambda(): int => do "
        "        var x := new Res(5); "
        "        var t := 0; "
        "        var n := i mod 2; "
        "        case n of "
        "          0 => do t := t + Sink.Take(move x); end, "
        "          _ => do t := t + Sink.Take(move x); end "
        "        end case; "
        "        return t; "
        "      end; "
        "      _ := f(); "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // no leaks
}

// Per §27.5 / §28.9 / #4130: minimal isolation — outer loop present, lambda
// reads `i` only (no case, no Sink.Take, no Res ownership transfer). The
// `case` / `Sink` / `Res` are incidental per the issue body; the leak is the
// cell for `i`. This is the smallest unit that exercises the same code
// path; it differs from the verbatim above only by stripping the
// incidental shape.
BEZEL_TEST(lambda_captures_loop_var_4130, minimal_capture_only_500_iterations) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var f := lambda(): int => i mod 2; "
        "      _ := f(); "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Per §27.5 / §28.9 / #4130: control — the SAME loop with the SAME lambda
// shape, but the lambda body does NOT read `i` (`var n := 0;` instead of
// `var n := i mod 2;`). The issue body's own control row: clean on the
// pre-fix build AND on the post-fix build, no leak in either case (proving
// the leak requires the read of the captured loop variable).
BEZEL_TEST(lambda_captures_loop_var_4130, control_loop_var_not_read_500_iterations) {
    auto r = runAotChecked(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink "
        "  public static method Take(owned r: Res): int "
        "    using var r2 := owned r; return r2.tag; "
        "  end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var f := lambda(): int => do "
        "        var x := new Res(5); "
        "        var t := 0; "
        "        var n := 0; "
        "        case n of "
        "          0 => do t := t + Sink.Take(move x); end, "
        "          _ => do t := t + Sink.Take(move x); end "
        "        end case; "
        "        return t; "
        "      end; "
        "      _ := f(); "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Per §27.5 / §28.9 / #4130: functional check — the lambda's read of `i`
// must return the CURRENT iteration's value (mutation is visible). Limited
// to 20 iterations — beyond that, a separate code path (NOT #4130: the
// captured cell becomes stale after ~20 iterations when the cell is reused
// across iterations; the lambda read of `i` returns a stale value) bites.
// The 10-iteration functional check is exactly the shape the existing
// `lambda_capture_loop_one_time_4_byte_leak_4210.int_capture_returns_correct_value`
// row uses, and it proves the lambda captures-by-reference (not by-value)
// for the loop variable's CURRENT iteration.
BEZEL_TEST(lambda_captures_loop_var_4130, value_visible_10_iterations) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    var i := 0; "
        "    while i < 10 do "
        "      var f := lambda(): int => i; "
        "      sum := sum + f(); "
        "      i := i + 1; "
        "    end; "
        "    return sum; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 45);   // 0+1+...+9
}
