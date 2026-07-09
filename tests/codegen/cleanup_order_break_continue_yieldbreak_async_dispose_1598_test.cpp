// fix #1598 — §29.7/§28.10 (v0.182): cleanup runs in REVERSE establishment
// order on EVERY exit edge, uniformly (follow-up to #1567, which fixed the
// RETURN edge, the sync ITERATOR early-dispose edge, and the ASYNC-METHOD
// return edge). This file covers the three edges #1567 deferred to keep that
// fix scoped:
//   (a) `break` that exits a `try`/`finally` inside a loop.
//   (b) `continue` that exits a `try`/`finally` inside a loop.
//   (c) `yield break` that exits a `try`/`finally` inside a sync iterator.
//   (d) the ASYNC-ITERATOR early-dispose edge (consumer abandons iteration
//       mid-suspend via `await for ... break`).
// A nested (nested `try`/`finally`) nested-chaining variant of (a) is included
// as a fifth test to guard the `outerFr` propagation path, which #1598 also
// touches.
//
// Spec authority: §28.10 ("using var disposals and explicit finally clauses
// form one establishment-ordered stack ... on every exit edge (normal,
// return, break/continue, exception, and an iterator's early disposal") and
// §29.7 ("[an iterator Dispose] runs the pending cleanup actions ... in
// reverse order of establishment, exactly as if the body had exited at the
// suspension point").
//
// Observation technique (same as #1567): a static sequence counter that each
// cleanup action folds a digit into (seq := seq*10 + tag). The finally reads
// the enclosing resource's field (a genuine touch) so a use-after-dispose
// either crashes or (for the yield-break edge, which pre-fix disposed nothing
// at all rather than disposing too early) silently omits the destructor's
// digit. Pre-fix behavior for each edge, confirmed against this fix's parent
// commit (the #1567 fix, before the #1598 patch) with the AOT-compiled
// binary: (a)/(b)/(d) segfault (use-after-dispose: the enclosing resource was
// freed via emitLoopDisposals/category-phase-disposal BEFORE the finally
// touched it); (c) returns 4 instead of 41 (the enclosing `using var` is never
// disposed at all on the pre-fix yield-break-through-finally edge — a leak,
// not a UAF, because that site called no disposal function whatsoever). Every
// post-fix case below is additionally valgrind-clean (dispose-once, no
// leaks).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// (a) §29.7/§28.10 (v0.182): BREAK edge. `Inner` loops; each iteration
// establishes `using var a` INSIDE the loop but OUTSIDE an inner
// `try { break } finally { touch a }`. The first iteration's break must run
// the finally (reads a.tag=4) while `a` is still alive, THEN dispose `a`
// (destructor +1) — THEN jump to the loop's break target. Correct order => 41.
// Pre-fix, the break handler called emitLoopDisposals(loopScopeDepth) — which
// disposes everything from the loop's own scope depth up, including `a` —
// BEFORE routing to the finally: a use-after-dispose reading a.tag on freed
// memory (segfaults on this project's allocator). Note an enclosing
// `using var` declared BEFORE the loop (rather than inside it) does NOT
// exercise this bug: its scope index is already below loopScopeDepth, so the
// pre-fix emitLoopDisposals call correctly excluded it; the bug requires the
// resource to be established between the loop's own scope depth and the
// try's entry depth, as here.
BEZEL_TEST(codegen_cleanup_order_1598,
           cleanup_order_break_edge_finally_before_dispose_spec_29_7) {
    BEZEL_EXPECT_EQ(run(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Inner(): int
    for var i in 0..3 do
      using var a := new A();
      try
        break;
      finally
        C._seq := C._seq * 10 + a.tag;
      end;
    end;
    return 0;
  end Inner;
  public static method Main(): int
    C._seq := 0;
    _ := Inner();
    return C._seq;
  end Main;
end C;
class A
  public tag: int := 4;
  destructor C._seq := C._seq * 10 + 1; end destructor;
end A;
)"), 41);
}

// (a2) §29.7/§28.10 (v0.182): BREAK edge through TWO nested finally frames —
// guards the `outerFr` propagation branch (mode-2 dispatch chaining to a
// further-outer finally) that #1598 also changed. `b` (tag=4) is established
// between the outer try's entry and the inner try; the inner finally (+4) then
// `b`'s destructor (+1) must both run BEFORE the outer finally (+9), which in
// turn runs before the loop is exited. Correct order: 4, 1, 9 => 419 (mod 256
// as an OS exit code = 163; this test uses the in-process JIT so it observes
// the untruncated value, 419).
BEZEL_TEST(codegen_cleanup_order_1598,
           cleanup_order_break_edge_nested_finally_chain_spec_29_7) {
    BEZEL_EXPECT_EQ(run(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Inner(): int
    for var i in 0..3 do
      try
        using var b := new B();
        try
          break;
        finally
          C._seq := C._seq * 10 + b.tag;
        end;
      finally
        C._seq := C._seq * 10 + 9;
      end;
    end;
    return 0;
  end Inner;
  public static method Main(): int
    C._seq := 0;
    _ := Inner();
    return C._seq;
  end Main;
end C;
class B
  public tag: int := 4;
  destructor C._seq := C._seq * 10 + 1; end destructor;
end B;
)"), 419);
}

// (b) §29.7/§28.10 (v0.182): CONTINUE edge. Mirrors (a) but with `continue`:
// each of the 3 loop iterations (range `0..2` is inclusive, §16.3) establishes
// a fresh `using var a` inside the loop, then `continue`s out of an inner
// `try`/`finally`. Each iteration's finally (+4) must run before that
// iteration's `a` is disposed (+1), repeated for all 3 iterations in order:
// seq = ((((0*10+4)*10+1)*10+4)*10+1)*10+4)*10+1 = 414141. Pre-fix: the
// continue handler's up-front emitLoopDisposals(loopScopeDepth) disposed that
// iteration's `a` before its finally read a.tag — a use-after-dispose
// (segfault).
BEZEL_TEST(codegen_cleanup_order_1598,
           cleanup_order_continue_edge_finally_before_dispose_spec_29_7) {
    BEZEL_EXPECT_EQ(run(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Inner(): int
    for var i in 0..2 do
      using var a := new A();
      try
        continue;
      finally
        C._seq := C._seq * 10 + a.tag;
      end;
    end;
    return 0;
  end Inner;
  public static method Main(): int
    C._seq := 0;
    _ := Inner();
    return C._seq;
  end Main;
end C;
class A
  public tag: int := 4;
  destructor C._seq := C._seq * 10 + 1; end destructor;
end A;
)"), 414141);
}

// (c) §29.7/§28.10 (v0.182): YIELD BREAK edge. `Gen` has an enclosing
// `using var a` and an inner `try { yield break } finally { touch a }`. The
// `yield break` handler must dispose the try-body scopes, run the finally
// (reads a.tag=4 while `a` is alive), THEN — in the finally's mode-4
// post-finally dispatch — dispose `a` (destructor +1) before completing the
// terminal iterator exit (state=-1, `MoveNext` returns false). Correct order
// => 41. Pre-fix this call site disposed NOTHING at all before jumping to the
// finally (unlike return/break/continue, which over-disposed), and the mode-4
// dispatch never disposed the enclosing range either — so `a` was never
// disposed on this edge at all pre-fix: the destructor's +1 never fired and
// the result was 4, not 41 (a leak, confirmed with valgrind pre-fix showing
// a definite-lost block; no crash because nothing touches freed memory on
// this edge, unlike (a)/(b)/(d)).
BEZEL_TEST(codegen_cleanup_order_1598,
           cleanup_order_yield_break_edge_finally_before_dispose_spec_29_7) {
    BEZEL_EXPECT_EQ(run(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Gen(): IEnumerable<int>
    using var a := new A();
    try
      yield break;
    finally
      C._seq := C._seq * 10 + a.tag;
    end;
  end Gen;
  public static method Main(): int
    C._seq := 0;
    for var x in Gen() do
    end;
    return C._seq;
  end Main;
end C;
class A
  public tag: int := 4;
  destructor C._seq := C._seq * 10 + 1; end destructor;
end A;
)"), 41);
}

// (d) §29.7/§28.10 (v0.182): ASYNC-ITERATOR early-dispose edge. `Gen` is an
// async iterator (`IAsyncEnumerable<int>`) with an enclosing `using var a`
// and an inner `try { yield return 1; yield return 2; } finally { touch a }`.
// The consumer (`await for`) abandons iteration after the first value,
// triggering the compiler-generated iterator `Dispose` while suspended inside
// the try (§29.7). `Dispose` must run the finally (reads a.tag=4) BEFORE
// disposing `a` (destructor +1). Correct order => 41 — identical to the SYNC
// iterator-dispose edge #1567 already fixed, since `Dispose` now consumes the
// same unified `smIterCleanupAtState_` snapshot for both. Pre-fix, the async
// `Dispose` ran ALL using-var disposals (disposing `a`) THEN ALL finally
// bodies as two category-ordered phases — the finally's a.tag read was a
// use-after-dispose (segfault), the same inversion #1567 fixed for the sync
// path.
BEZEL_TEST(codegen_cleanup_order_1598,
           cleanup_order_async_iterator_dispose_edge_finally_before_dispose_spec_29_7) {
    BEZEL_EXPECT_EQ(run(R"(
module M;
class C
  public static _seq: int := 0;
  public static async method Gen(): IAsyncEnumerable<int>
    using var a := new A();
    try
      yield return 1;
      yield return 2;
    finally
      C._seq := C._seq * 10 + a.tag;
    end;
  end Gen;
  public static async method Main(): Task<int>
    C._seq := 0;
    await for var x in Gen() do
      if x = 1 then break; end;
    end;
    return C._seq;
  end Main;
end C;
class A
  public tag: int := 4;
  destructor C._seq := C._seq * 10 + 1; end destructor;
end A;
)"), 41);
}
