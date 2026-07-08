// fix #1567 — §29.7/§28.10 (v0.182): cleanup runs in REVERSE establishment
// order on EVERY exit edge, uniformly. `using var` disposals and explicit
// `finally` clauses form ONE establishment-ordered stack — no edge runs the two
// kinds in separate category-ordered phases (§28.10). For an enclosing
// `using var a` followed by an inner `try/finally F` (F established after a), a
// finally that touches `a` must run BEFORE `a` is disposed. The pre-v0.182
// codegen inverted this on the RETURN edge and the ITERATOR early-dispose edge
// (§29.7): it disposed `a` first, then ran F — a use-after-dispose. The
// EXCEPTION edge was already correct; the third test guards that it stays so.
//
// Observation technique (same as the #1263 ordering test): a static sequence
// counter that each cleanup action folds a digit into (seq := seq*10 + tag).
// The finally reads `a.tag` — a field of the enclosing `using var` — so it is a
// genuine touch of the resource: if `a` were disposed first, the read is a
// use-after-dispose (the pre-fix code crashes / reads freed memory here). The
// final counter value encodes the exact order:
//   correct (finally F THEN dispose a):  seq = 0*10+4 (F reads a.tag=4)
//                                             then *10+1 (a's destructor) = 41
//   inverted (dispose a THEN finally F):  a freed, then F reads a.tag = UAF.
//
// This is DISTINCT from #1263 (owned locals INSIDE the finally's own try body,
// which are disposed BEFORE the finally on the exception edge — unchanged here):
// #1567 is about an ENCLOSING `using var` established before the inner try.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// (a) §29.7/§28.10 (v0.182): RETURN edge. `Inner` has an enclosing
// `using var a` and an inner `try { return } finally { touch a }`. On the
// return edge the finally (which reads a.tag=4) must run while `a` is still
// alive, THEN `a` is disposed (destructor folds +1). Correct order => 41.
// Pre-fix the return edge disposed `a` before the finally: reading a.tag was a
// use-after-dispose (crash). Main observes the counter after Inner returns.
BEZEL_TEST(codegen_cleanup_order_1567,
           cleanup_order_return_edge_finally_before_dispose_spec_29_7) {
    BEZEL_EXPECT_EQ(run(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Inner(): int
    using var a := new A();
    try
      return 0;
    finally
      C._seq := C._seq * 10 + a.tag;
    end;
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

// (b) §29.7/§28.10 (v0.182): ITERATOR early-dispose edge. `Gen` has an
// enclosing `using var a` and an inner `try { yield } finally { touch a }`. The
// consumer breaks after the first element, so the iterator is disposed while
// suspended inside the try (§29.7). The pending cleanup must run the finally
// (reads a.tag=4) BEFORE disposing `a` (destructor +1). Correct order => 41.
// Pre-fix the iterator Dispose ran all using-var disposals, then all finally
// bodies (category phases): it disposed `a` first, then the finally read a.tag
// on the freed object — a use-after-dispose (crash).
BEZEL_TEST(codegen_cleanup_order_1567,
           cleanup_order_iterator_dispose_edge_finally_before_dispose_spec_29_7) {
    BEZEL_EXPECT_EQ(run(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Gen(): IEnumerable<int>
    using var a := new A();
    try
      yield return 1;
      yield return 2;
    finally
      C._seq := C._seq * 10 + a.tag;
    end;
  end Gen;
  public static method Main(): int
    C._seq := 0;
    for var x in Gen() do
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

// (b2) §29.7/§28.10 (v0.182): the ASYNC-METHOD return edge (exit mode 5, #721).
// An async method's `return` through a finally is the async twin of (a): it
// must run the finally (reading a.tag=4 while `a` is alive) BEFORE disposing
// `a` (destructor +1). Correct order => 41. Pre-fix the async-return edge ran
// emitAllDisposals() before the finally, freeing `a` first — the finally's
// a.tag read was a use-after-dispose (crash). `Run` awaits once so it is a real
// async state machine; `Main` awaits `Run` and observes the counter.
BEZEL_TEST(codegen_cleanup_order_1567,
           cleanup_order_async_return_edge_finally_before_dispose_spec_29_7) {
    BEZEL_EXPECT_EQ(run(R"(
module M;
class C
  public static _seq: int := 0;
  public static async method Delay(): Task<int> return 9; end Delay;
  public static async method Run(): Task<int>
    var w: int := await C.Delay();
    using var a := new A();
    try
      return 0;
    finally
      C._seq := C._seq * 10 + a.tag;
    end;
  end Run;
  public static async method Main(): Task<int>
    C._seq := 0;
    var r: int := await C.Run();
    return C._seq;
  end Main;
end C;
class A
  public tag: int := 4;
  destructor C._seq := C._seq * 10 + 1; end destructor;
end A;
)"), 41);
}

// (c) §29.7/§28.10 (v0.182) regression guard: the EXCEPTION edge was already
// correct (reverse establishment) and MUST stay so. Inner `try { throw }
// finally { touch a }` inside an outer `try/catch`, with an enclosing
// `using var a`. On the throw the inner finally runs first (reads a.tag=1 while
// `a` is alive), then `a` is disposed (destructor +4), then the outer catch
// runs (+7): seq = ((0*10+1)*10+4)*10+7 = 147. This value is unchanged by the
// fix (the fix does not touch the exception/unwind path); a future regression
// that inverted the exception edge would change it (or crash on the a.tag read).
BEZEL_TEST(codegen_cleanup_order_1567,
           cleanup_order_exception_edge_reverse_regression_spec_28_10) {
    BEZEL_EXPECT_EQ(run(R"(
module M;
class E end E;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    try
      using var a := new A();
      try
        throw new E();
      finally
        C._seq := C._seq * 10 + a.tag;
      end;
    catch E => C._seq := C._seq * 10 + 7;
    end;
    return C._seq;
  end Main;
end C;
class A
  public tag: int := 1;
  destructor C._seq := C._seq * 10 + 4; end destructor;
end A;
)"), 147);
}
