// Regression tests for #6847 (§21.5, v0.340): a `break` (or `continue`)
// inside a `try`/`finally` nested in a `while` skipped the statement AFTER
// the loop when the loop itself sat inside another `try`.
//
// Spec §21.5 (v0.340): "`break` exits a loop; `continue` skips the rest of
// the current iteration and advances to the next." and "On the way to its
// target, the jump exits every intervening scope — inner loop bodies,
// `do … end` blocks, `try` bodies — and each performs its ordinary exit
// obligations". The ENCLOSING `try` is not an intervening scope of a break
// that leaves a loop INSIDE that try's protected block: the break lands on
// the statement after the loop, which is still inside the enclosing try's
// body, and the enclosing `finally` runs later — when that protected block
// completes normally (§31.2, v0.340).
//
// The bug. A finally's exit-mode dispatcher (mode 2 = break, mode 3 =
// continue) chained the jump to the ENCLOSING finally UNCONDITIONALLY, so
// the jump never landed on the target loop's exit block — the target was
// lost. An enclosing finally that had no loop of its own (one entered
// before any loop was open) captured a NULL break target, so ITS dispatcher
// had no mode-2 case at all and the switch fell through to its default
// (the statement after the enclosing try): the code after the loop was
// skipped entirely, and the enclosing finally ran early — before the code
// the jump was supposed to reach.
//
// The fix routes the jump to the enclosing finally only when that finally
// lies INSIDE the target loop (the same test the labeled-route chain
// applies); otherwise the jump lands on the target loop's exit block.
//
// The observables are on stdout (the exit code is 8-bit truncated, so the
// trace counts would not fit in it — CLAUDE.md, "Test and oracle rules").

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string stdoutOf(const std::string& prog) {
    auto r = runAot(prog);
    if (!r.ran) return "<program did not run>";
    return r.stdoutText;
}
}  // namespace

// #6847: the issue's own repro, verbatim in shape. The `break` leaves the
// `while` through the inner `finally`; the statement after the loop runs
// (still inside the outer try's body) and the outer `finally` runs once, at
// the end of that body.
BEZEL_TEST(break_through_finally_in_loop_6847,
           break_in_finally_in_loop_inside_try_runs_after_loop_code) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; class C "
        "  public static method Main(): int "
        "    try "
        "      var i := 0; "
        "      while i < 3 do "
        "        i := i + 1; "
        "        try "
        "          if i = 2 then break; end; "
        "        finally "
        "          Console.WriteLine(\"fin \" + i.ToString()); "
        "        end try; "
        "        Console.WriteLine(\"body \" + i.ToString()); "
        "      end while; "
        "      Console.WriteLine(\"after loop \" + i.ToString()); "
        "    finally "
        "      Console.WriteLine(\"outer\"); "
        "    end try; "
        "    return 7; "
        "  end Main; "
        "end C;"),
        "fin 1\nbody 1\nfin 2\nafter loop 2\nouter\n");
}

// #6847: the `continue` twin. The `continue` advances the loop, so the
// remaining iterations run; the statement after the loop and the enclosing
// `finally` follow in order.
BEZEL_TEST(break_through_finally_in_loop_6847,
           continue_in_finally_in_loop_inside_try_runs_after_loop_code) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; class C "
        "  public static method Main(): int "
        "    try "
        "      var i := 0; "
        "      while i < 4 do "
        "        i := i + 1; "
        "        try "
        "          if i = 2 then continue; end; "
        "        finally "
        "          Console.WriteLine(\"fin \" + i.ToString()); "
        "        end try; "
        "        Console.WriteLine(\"body \" + i.ToString()); "
        "      end while; "
        "      Console.WriteLine(\"after loop \" + i.ToString()); "
        "    finally "
        "      Console.WriteLine(\"outer\"); "
        "    end try; "
        "    return 7; "
        "  end Main; "
        "end C;"),
        "fin 1\nbody 1\nfin 2\nfin 3\nbody 3\nfin 4\nbody 4\nafter loop 4\nouter\n");
}

// #6847: the CONTROL — the same program with the inner try/finally removed.
// The `break` behaves identically, which is what makes the inner try the
// only difference between the broken and the correct behaviour.
BEZEL_TEST(break_through_finally_in_loop_6847,
           control_same_program_without_the_inner_try) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; class C "
        "  public static method Main(): int "
        "    try "
        "      var i := 0; "
        "      while i < 3 do "
        "        i := i + 1; "
        "        if i = 2 then break; end; "
        "        Console.WriteLine(\"body \" + i.ToString()); "
        "      end while; "
        "      Console.WriteLine(\"after loop \" + i.ToString()); "
        "    finally "
        "      Console.WriteLine(\"outer\"); "
        "    end try; "
        "    return 7; "
        "  end Main; "
        "end C;"),
        "body 1\nafter loop 2\nouter\n");
}

// #6847: an enclosing finally that lies OUTSIDE the target loop. The break
// leaves the inner loop but stays inside the enclosing try's body, so the
// code after the inner loop AND the code after the outer loop both run, and
// the enclosing finally runs once, at the end of its protected block.
BEZEL_TEST(break_through_finally_in_loop_6847,
           enclosing_finally_outside_the_target_loop_runs_last) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; class C "
        "  public static method Main(): int "
        "    var trace := 0; "
        "    try "
        "      var j := 0; "
        "      while j < 2 do "
        "        j := j + 1; "
        "        while true do "
        "          try "
        "            trace := trace + 1; "
        "            break; "
        "          finally trace := trace + 10; end; "
        "        end; "
        "        trace := trace + 100; "
        "      end; "
        "      trace := trace + 1000; "
        "    finally trace := trace + 10000; end; "
        "    Console.WriteLine(trace.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "11222\n");
}

// #6847: the SOUNDNESS twin. A `using var` established in the enclosing
// try's body must still be ALIVE at the statement after the loop — the
// break does not leave the enclosing try's protected block. Pre-fix the
// break edge disposed the enclosing try's BODY scope (the chain's
// disposal range reached past the target loop's scope depth), so the
// resource was released before the code after the loop could use it.
BEZEL_TEST(break_through_finally_in_loop_6847,
           resource_of_the_enclosing_try_body_survives_the_break) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; class Res "
        "  public N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  destructor Console.WriteLine(\"dispose \" + N.ToString()); end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(): int "
        "    try "
        "      using var r := new Res(1); "
        "      var i := 0; "
        "      while i < 3 do "
        "        i := i + 1; "
        "        try "
        "          if i = 2 then break; end; "
        "        finally Console.WriteLine(\"fin \" + i.ToString()); end try; "
        "      end while; "
        "      Console.WriteLine(\"after loop r=\" + r.N.ToString()); "
        "    finally Console.WriteLine(\"outer\"); end try; "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "fin 1\nfin 2\nafter loop r=1\ndispose 1\nouter\n");
}

// #6847: an enclosing finally that lies INSIDE the target loop. Here the
// break leaves the enclosing try's protected block as well, so the enclosing
// finally runs ON THE BREAK EDGE — the chain the fix preserves.
BEZEL_TEST(break_through_finally_in_loop_6847,
           enclosing_finally_inside_the_target_loop_runs_on_the_break_edge) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; class C "
        "  public static method Main(): int "
        "    var trace := 0; "
        "    while true do "
        "      try "
        "        try "
        "          trace := trace + 1; "
        "          break; "
        "        finally trace := trace + 10; end; "
        "      finally trace := trace + 100; end; "
        "    end; "
        "    trace := trace + 1000; "
        "    Console.WriteLine(trace.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "1111\n");
}
