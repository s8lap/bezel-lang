// #2051 — §21.5 (v0.201): labeled `break`/`continue` (proposal 0050).
//
// A `while`/`for`/`await for` may carry a loop label (`scan: for … end scan;`);
// `break scan;` exits the labeled loop and `continue scan;` advances it, from any
// depth of nesting within the same callable body. On the way out the jump exits
// every intervening scope, running each scope's disposal obligations (`using var`
// and pending `finally`) in REVERSE order of establishment — the v0.182 uniform
// exit-edge ordering (§28.10) — and the labeled forms "add or alter no disposal
// rule": they reuse the multi-scope unwind edge already defined for `return`,
// `throw`, and unlabeled `break`, only starting it from a farther target.
//
// Spec authority: §21.5 ("Loop labels" / "Labeled forms": `break name;` exits the
// enclosing loop labeled `name`; `continue name;` transfers to its advance point —
// the index/step increment on a counted loop; the jump "exits every intervening
// scope … each performs its ordinary exit obligations … in reverse order of
// establishment"); §29.2 (v0.201: the label rides the for-desugaring onto the
// generated `while`; a labeled break/continue is a body-exit edge for every loop
// body it exits).
//
// Disposal-order observation (same technique as #1567/#1598): a static `_seq`
// counter each destructor / finally folds a digit into (seq := seq*10 + tag).
// A wrong order, a missed disposal, or a double-dispose all produce a different
// value; every case here is additionally AOT-ASan-clean (dispose-exactly-once).
// Multi-digit sequences exceed the 8-bit exit-code ceiling, so those cases print
// and assert on stdout via runAotOut; small results round-trip as exit codes.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
// Generated-code ASan/LSan net: a wrong disposal order still folds to a
// different value, but a double-dispose / use-after-free / leak fails HERE
// (ran=false) rather than silently. Used for the disposal-order pins whose
// value fits an 8-bit exit code. (The finally-crossing pins fold a value beyond
// 255 and use the runAotOut value channel; the authoritative --full gate forces
// suite-wide ASan on those, and they were additionally verified ASan-clean.)
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §21.5: `break scan;` from the inner loop exits BOTH loops. Sum before the
// break: i=0 -> j=0,1,2 add (0+0),(0+1),(0+2); i=1 -> j=0 add (10+0); then
// i=1,j=1 breaks scan. 0+1+2+10 = 13.
BEZEL_TEST(labeled_break_continue_2051, break_exits_two_loops_spec_21_5) {
    BEZEL_EXPECT_EQ(run(R"(
module M;
class C
  public static method Main(): int
    var s := 0;
    scan: for var i in 0..2 do
      for var j in 0..2 do
        if i = 1 and j = 1 then break scan; end;
        s := s + i * 10 + j;
      end;
    end scan;
    return s;
  end Main;
end C;
)"), 13);
}

// §21.5: `continue scan;` skips the rest of the OUTER loop's current iteration
// and advances it. The trailing outer-body marker (+100 per completed outer
// iteration) never runs — every outer iteration hits the continue at j=1.
// i=0,1,2 each add (i*10+0) before continuing: 0+10+20 = 30.
BEZEL_TEST(labeled_break_continue_2051, continue_advances_outer_spec_21_5) {
    BEZEL_EXPECT_EQ(run(R"(
module M;
class C
  public static method Main(): int
    var s := 0;
    scan: for var i in 0..2 do
      for var j in 0..2 do
        if j = 1 then continue scan; end;
        s := s + i * 10 + j;
      end;
      s := s + 100;
    end scan;
    return s;
  end Main;
end C;
)"), 30);
}

// §21.5: a labeled `while`/`while` pair — `break scan;` exits both. Returns the
// value of (i, k) captured at the break: i=1, k=1 -> 11.
BEZEL_TEST(labeled_break_continue_2051, break_exits_two_while_loops_spec_21_5) {
    BEZEL_EXPECT_EQ(run(R"(
module M;
class C
  public static method Main(): int
    var i := 0;
    var last := -1;
    scan: while i < 3 do
      var k := 0;
      while k < 3 do
        if i = 1 and k = 1 then last := i * 10 + k; break scan; end;
        k := k + 1;
      end;
      i := i + 1;
    end scan;
    return last;
  end Main;
end C;
)"), 11);
}

// §21.5 / §29.2 (v0.201): `continue scan;` on a COUNTED (`step`) outer loop hits
// the loop's ADVANCE point — the index/step increment — not the bare condition;
// targeting the condition would skip an advance and diverge (an infinite loop).
// The inner loop always continues the outer at j=0, so the outer runs exactly
// once per step value: 0,2,4 over `0..<6 step 2` -> 3 iterations.
BEZEL_TEST(labeled_break_continue_2051, continue_counted_loop_hits_advance_spec_29_2) {
    BEZEL_EXPECT_EQ(run(R"(
module M;
class C
  public static method Main(): int
    var count := 0;
    scan: for var i in 0..<6 step 2 do
      count := count + 1;
      for var j in 0..2 do
        if j = 0 then continue scan; end;
      end;
    end scan;
    return count;
  end Main;
end C;
)"), 3);
}

// §21.5 / §28.10 (v0.182): DISPOSAL ORDER across a multi-level labeled break.
// `a` (destructor +1) is an outer-body `using var`; `b` (+2) an inner-body
// `using var` established later. On `break scan;` the jump exits both bodies and
// disposes in reverse establishment order: b THEN a -> seq (0*10+2)*10+1 = 21.
// A wrong order (12), a missed disposal, or a double-dispose all differ; the run
// is ASan-clean (each destructor fires exactly once).
BEZEL_TEST(labeled_break_continue_2051, break_disposal_order_reverse_establishment_spec_28_10) {
    BEZEL_EXPECT_EQ(runChecked(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    scan: for var i in 0..0 do
      using var a := new A();
      for var j in 0..0 do
        using var b := new B();
        break scan;
      end;
    end scan;
    return C._seq;
  end Main;
end C;
class A destructor C._seq := C._seq * 10 + 1; end destructor; end A;
class B destructor C._seq := C._seq * 10 + 2; end destructor; end B;
)"), 21);
}

// §21.5 / §31.2 (v0.182): a labeled break to an OUTER loop that CROSSES a
// `finally` — and has a loop BETWEEN the target and the try (so the fixed
// mode-2/3 dispatch can't name the target). The finally must run interleaved
// with the reverse-establishment disposal chain: the try-body `using var c`
// (+3) is disposed first, then the finally (+9) runs, then the enclosing
// scopes dispose after it — inner `b` (+2), outer `a` (+1). Order: 3,9,2,1
// -> 3921. Pre-fix the labeled break did not parse at all.
BEZEL_TEST(labeled_break_continue_2051, break_across_finally_to_outer_loop_spec_31_2) {
    BEZEL_EXPECT_EQ(runAotOut(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    scan: for var i in 0..0 do
      using var a := new A();
      for var j in 0..0 do
        using var b := new B();
        try
          using var c := new Cc();
          break scan;
        finally
          C._seq := C._seq * 10 + 9;
        end;
      end;
    end scan;
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class A destructor C._seq := C._seq * 10 + 1; end destructor; end A;
class B destructor C._seq := C._seq * 10 + 2; end destructor; end B;
class Cc destructor C._seq := C._seq * 10 + 3; end destructor; end Cc;
)"), "3921");
}

// §21.5 / §31.2: a labeled CONTINUE to an outer loop across a `finally`, run for
// two outer iterations. Each iteration: c (+3) disposed at the site, finally
// (+9), then b (+2) and a (+1) after it; the trailing outer-body marker (+7) is
// SKIPPED by `continue scan`. Two iterations: 3921 3921 -> 39213921.
BEZEL_TEST(labeled_break_continue_2051, continue_across_finally_to_outer_loop_spec_31_2) {
    BEZEL_EXPECT_EQ(runAotOut(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    scan: for var i in 0..1 do
      using var a := new A();
      for var j in 0..0 do
        using var b := new B();
        try
          using var c := new Cc();
          continue scan;
        finally
          C._seq := C._seq * 10 + 9;
        end;
      end;
      C._seq := C._seq * 10 + 7;
    end scan;
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class A destructor C._seq := C._seq * 10 + 1; end destructor; end A;
class B destructor C._seq := C._seq * 10 + 2; end destructor; end B;
class Cc destructor C._seq := C._seq * 10 + 3; end destructor; end Cc;
)"), "39213921");
}

// §21.5 / §30.7 (v0.201, divergence 0.1.1): a labeled `await for` in an async
// method; a `break scan;` from an inner loop exits the async iteration on its
// normal edge. Sum before the break: v=1 -> j=0,1,2 add (10+0),(10+1),(10+2);
// v=2 -> j=0 add (20+0); then v=2,j=1 breaks scan. 10+11+12+20 = 53.
BEZEL_TEST(labeled_break_continue_2051, labeled_await_for_break_spec_30_7) {
    BEZEL_EXPECT_EQ(run(R"(
module M;
class C
  public static async method Gen(items: Array<int>): IAsyncEnumerable<int>
    for var x in items do yield return x; end;
  end Gen;
  public static async method Main(): Task<int>
    using var a := new Array<int>();
    a.Add(1); a.Add(2); a.Add(3);
    var sum := 0;
    scan: await for var v in Gen(a) do
      for var j in 0..2 do
        if v = 2 and j = 1 then break scan; end;
        sum := sum + v * 10 + j;
      end;
    end scan;
    return sum;
  end Main;
end C;
)"), 53);
}
