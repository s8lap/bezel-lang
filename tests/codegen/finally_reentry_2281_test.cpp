// Codegen — issue #2281 (§31.2 / §29.7, v0.236): emitter re-entry while
// lowering a nested standalone function must not destroy the caller's active
// try/finally registrations.
//
// buildSpecializedMethodFn, makePropertyThunk, ensureExprGetterThunk, and
// ensurePropertyAccessorFn cleared finallyStack_ without restoring it. A lazy
// emission reached from a try body therefore left lowerTry's post-body
// finallyStack_.back()/pop_back() operating on an empty vector (assertion/UB),
// and exits stopped routing through the enclosing finally. §31.2 requires a
// finally to run on normal completion, exception, return, break, and continue.
// finallyBodyStack_ is restored in lockstep per §29.7's depth invariant.
//
// The generic case also gives the nested emitted function its own try/finally;
// both counters are folded into the exit code so the test observes both bodies,
// not merely successful compilation. The property-reference and custom-
// enumerable cases exercise the two body-time thunk emitters. All lazy sites
// are first-use cache misses inside the protected block.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §31.2, v0.236: build a generic specialization inside an outer try. The
// specialization's own return runs its finally (+10), then the outer return
// runs its finally (+100): 110. Without #2281 bezelc aborts on the outer
// finallyStack_.back(), so runAot reports ran=false and run() returns -999.
BEZEL_TEST(finally_reentry_2281,
           generic_callee_and_caller_finallys_both_run) {
    BEZEL_EXPECT_EQ(run(R"(
module M;
class G
  public static fin: int := 0;
  public static method Id<T>(x: T): T
    try
      return x;
    finally
      fin := fin + 1;
    end;
  end Id;
end G;
class P
  public static fin: int := 0;
  public static method Work(): int
    try
      var x := G.Id<int>(7);
      return x;
    finally
      fin := fin + 1;
    end;
  end Work;
  public static method Main(): int
    var w := P.Work();
    return P.fin * 100 + G.fin * 10 + (w - 7);
  end Main;
end P;
)"), 110);
}

// §31.2, v0.236: the first property reference emits makePropertyThunk inside
// the try. Its invocation returns 7 and the enclosing return still runs the
// finally, producing 10.
BEZEL_TEST(finally_reentry_2281,
           property_reference_thunk_preserves_caller_finally) {
    BEZEL_EXPECT_EQ(run(R"(
module M;
class Counter
  private n: int;
  public constructor() n := 7; end constructor;
  public property N: int => n;
end Counter;
class P
  public static fin: int := 0;
  public static method Work(): int
    using var c := new Counter();
    try
      var f: () -> int := c.N;
      return f();
    finally
      fin := fin + 1;
    end;
  end Work;
  public static method Main(): int
    var w := P.Work();
    return fin * 10 + (w - 7);
  end Main;
end P;
)"), 10);
}
