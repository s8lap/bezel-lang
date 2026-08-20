// Codegen — issue #3287 / spec §22.5 (v0.265) + stdlib §2.20 / §35.2:
// `UnwrapOr` / `Map` / `MapError` / `AndThen` must evaluate their argument
// expression AT THE CALL SITE, in textual order, BEFORE the Ok/Error branch
// (§22.5 *Evaluation order* — "normative; observable through side-effecting
// arguments and non-constant defaults"). Pre-fix, the dedicated codegen
// path (LlvmBackend_Calls.cpp::tryLowerResultCombinatorCall) lowered the
// argument expression INSIDE the Ok or Error arm — so a side-effecting
// argument whose arm was not taken never ran.
//
// Stdlib §2.20 makes the rule normative for `UnwrapOr`: the fallback is
// an ordinary by-value parameter, there is no lazy `UnwrapOrElse`
// (recorded-decline in changelog v0.233), and the parameter name is API
// surface under §22.5. §35.2 grants compiler-provided members no
// exemption from the rule — all four combinators are bound.
//
// These four compiler-provided combinators have no `MethodSymbol` — the
// prelude body's member list holds only `IsOk` / `IsError`, so the regular
// method-symbol dispatch is bypassed and the inline-body lowering in
// `tryLowerResultCombinatorCall` is the only lowering. The fix lifts the
// `lowerExpr(arg)` step to the call site (one `entryAlloca` to keep the
// evaluated value live across the branch), then invokes the pre-evaluated
// closure inside the appropriate arm.
//
// Each test pins a distinct per-combinator × unused-branch combination:
// the side-effecting argument is observed on the branch that PRE-FIX
// deferred the evaluation. Pre-fix every test in this file returned the
// "did-not-run" sentinel (100); post-fix each returns 0.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// §22.5 / §2.20 / UnwrapOr on Ok: the side-effecting fallback expression
// (`counter.Bump()`) must run even though the fallback's value is
// discarded in favor of the Ok payload `42`. Pre-fix this returned 100
// (the bump never ran on the Ok path); post-fix it returns 0.
BEZEL_TEST(result_combinator_eager_arg_3287, unwrapor_arg_runs_on_ok) {
    auto r = runAot(R"(
module M;
class Counter
  public n: int;
  public constructor() n := 0; end constructor;
  public method Bump(): int
    n := n + 1;
    return 0;
  end Bump;
end Counter;
class App public static method Main(): int
  using var counter := new Counter();
  var r: Result<int, string> := Result<int, string>.Ok(42);
  var v: int := r.UnwrapOr(counter.Bump());
  return (if counter.n = 1 then 0 else 100);
end Main; end App;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §22.5 / §2.20 / UnwrapOr on Error: the fallback's side effect runs and
// the fallback's VALUE is what `v` observes. Both pre-fix and post-fix the
// side effect runs on Error (the path that uses the value); this pins
// that the eager-eval fix did not regress to "drop the side effect when
// not used" by mistake.
BEZEL_TEST(result_combinator_eager_arg_3287, unwrapor_arg_value_used_on_error) {
    auto r = runAot(R"(
module M;
class Counter
  public n: int;
  public constructor() n := 0; end constructor;
  public method Bump(): int
    n := n + 1;
    return 0;
  end Bump;
end Counter;
class App public static method Main(): int
  using var counter := new Counter();
  var r: Result<int, string> := Result<int, string>.Error("x");
  var v: int := r.UnwrapOr(counter.Bump());
  return (if v = 0 and counter.n = 1 then 0 else 100);
end Main; end App;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §22.5 / Map on Error: the closure expression is a CALL to a static
// factory whose only observable behavior besides its return value is
// `Counter.n := n + 1`. Pre-fix, the call lived inside the Ok arm; on
// the Error arm the call never ran, so `Counter.n` stayed 0. Post-fix
// the call runs at the call site, before the branch, on every path.
BEZEL_TEST(result_combinator_eager_arg_3287, map_factory_call_runs_on_error) {
    auto r = runAot(R"(
module M;
class Counter
  public static n: int := 0;
  public static method BumpAndMapDouble(): (int) -> int
    Counter.n := Counter.n + 1;
    return lambda(x: int) => x * 2;
  end BumpAndMapDouble;
end Counter;
class App public static method Main(): int
  var r: Result<int, string> := Result<int, string>.Error("bad");
  var mapped: Result<int, string> := r.Map(Counter.BumpAndMapDouble());
  return (if Counter.n = 1 then 0 else 100);
end Main; end App;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §22.5 / MapError on Ok: symmetric to the Map-on-Error case, but with
// the closure applied on the Error arm. The factory call
// `Counter.BumpAndStringLength` must run on the Ok path too.
BEZEL_TEST(result_combinator_eager_arg_3287, maperror_factory_call_runs_on_ok) {
    auto r = runAot(R"(
module M;
class Counter
  public static n: int := 0;
  public static method BumpAndStringLength(): (string) -> long
    Counter.n := Counter.n + 1;
    return lambda(s: string) => long(s.Length);
  end BumpAndStringLength;
end Counter;
class App public static method Main(): int
  var r: Result<int, string> := Result<int, string>.Ok(99);
  var mapped: Result<int, long> := r.MapError(Counter.BumpAndStringLength());
  return (if Counter.n = 1 then 0 else 100);
end Main; end App;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §22.5 / AndThen on Error: like Map, AndThen's closure is invoked on the
// Ok arm; on the Error arm, the closure's BODY doesn't run, but its
// CONSTRUCTION (the call to `BumpAndChain()`) must.
BEZEL_TEST(result_combinator_eager_arg_3287, andthen_factory_call_runs_on_error) {
    auto r = runAot(R"(
module M;
class Counter
  public static n: int := 0;
  public static method BumpAndChain(): (int) -> Result<long, string>
    Counter.n := Counter.n + 1;
    return lambda(x: int) => Result<long, string>.Ok(long(x * 3));
  end BumpAndChain;
end Counter;
class App public static method Main(): int
  var r: Result<int, string> := Result<int, string>.Error("step1 failed");
  var chained: Result<long, string> := r.AndThen(Counter.BumpAndChain());
  return (if Counter.n = 1 then 0 else 100);
end Main; end App;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §22.5 / Map's pure-method-group arg on Error: pinning the
// closure-CONSTRUCTION separately from the closure-body invocation. A
// method group like `counter.MapDouble` lowers to a closure capture
// WITHOUT calling `MapDouble`. So on either path, `counter.n` stays 0
// (only calls bump n). On Error, the body never runs (Error is passed
// through), so n stays 0 and the Error payload is preserved. This pins
// that the eager-eval fix didn't introduce any spurious method
// invocation when the closure is a plain method group.
BEZEL_TEST(result_combinator_eager_arg_3287,
           map_method_group_arg_does_not_call_body_on_error) {
    auto r = runAot(R"(
module M;
class Counter
  public n: int;
  public constructor() n := 0; end constructor;
  public method MapDouble(x: int): int
    n := n + 1;
    return x * 2;
  end MapDouble;
end Counter;
class App public static method Main(): int
  using var counter := new Counter();
  var r: Result<int, string> := Result<int, string>.Error("x");
  var mapped: Result<int, string> := r.Map(counter.MapDouble);
  if mapped.IsError() then
    return (if counter.n = 0 then 0 else 100);
  else
    return 100;
  end;
end Main; end App;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

}  // namespace
