// narrow #2355 — stdlib §2.20 / spec §35.2 + §27.4 (v0.307): an UN-ANNOTATED
// lambda argument to a `Result<T, E>` value-payload combinator must take its
// parameter type from the combinator's own signature.
//
// THE RULE, read off the documents before the fix was written:
//
//   * stdlib §2.20 declares `Map<U>(f: (T) -> U)`, `MapError<F>(f: (E) -> F)`
//     and `AndThen<U>(f: (T) -> Result<U, E>)`. The parameter type of the
//     function argument is therefore fixed by the RECEIVER instantiation: T
//     for `Map`/`AndThen`, E for `MapError`.
//   * §27.4: a lambda parameter's type annotation may be omitted "whenever
//     the target type is known from context"; only "in top-level contexts
//     with no target type ... parameter types must be specified".
//   * stdlib §2.20's own worked examples spell it that way —
//     `ParsePort(raw).Map(lambda(p) => $"listening on {p}")` and
//     `ParsePort(raw).MapError(lambda(m) => new ConfigError("port", m))` —
//     so the un-annotated form is the documented idiom, not an edge case.
//
// WHAT WENT WRONG. The four combinators are COMPILER-PROVIDED: the prelude's
// `Result` member list holds only `IsOk`/`IsError`, so there is no
// `ast::MethodDecl` and no `sema::MethodSymbol` for `Map`. The two threads
// that give a call-argument lambda its parameter types — `formalFnParamType2`
// (fix #829, extended by #4510) and the singular `pendingLambdaParamType_`
// (§24 event payload / stdlib §5 LINQ element) — both read a declared formal,
// so neither fired. The lambda's parameter stayed untyped, its inferred
// function TypeRef never materialised, and the combinator arm in
// `visitCallExpr` rejected the whole call:
//
//   `Result.Map` requires a function-typed argument (§35.2)
//
// Measured on 921dbc45 (docs v0.307) with the real driver: the three rows
// below were each REJECTED at compile time; re-spelling the very same
// programs with `lambda(x: int)` / `lambda(e: string)` compiled and printed
// the spec's values. So the defect was purely the missing target-type
// thread, not the combinator semantics.
//
// THE FIX adds the third singular thread — `resultCombinatorLambdaParamType`
// in ExprTypecheck_Calls.cpp — alongside the event-handler and LINQ ones, and
// consults it in the same `else if` chain in ExprTypecheck_Exprs.cpp. It is
// deliberately narrow: `UnwrapOr` is excluded (§35.2 makes its `fallback` an
// ordinary value, not a callback — "there is no lazy UnwrapOrElse"), and a
// non-`Result` receiver returns null, so no other call is reached.
//
// THE ORACLES RUN THE PROGRAM. Per the "a fix that makes something newly
// ACCEPTED must be RUN, not just compiled" rule, every accept row below
// asserts the program's captured stdout, not `ran`/`exitCode` alone: a
// silently-wrong accept is the expensive failure mode for an over-rejection
// fix, and `Result`'s codegen dispatch is structural
// (`tryLowerResultCombinatorCall`), so a correct sema symbol is not by itself
// evidence the lowering agrees.
//
// TEST MATRIX:
//   T1-T3  the three rule-corpus rows this fix moves — l2_20_0002 (Map over
//          Ok), l2_20_0003 (Map passes Error through), l2_20_0004 (MapError
//          over Error) — verbatim programs, asserting their `//! stdout:`.
//   T4     `MapError` on the Ok path: the E-typed parameter thread is
//          exercised even when the arm is not taken (the payload rides
//          through unchanged).
//   T5     the ANNOTATED spelling still works — the fix must not displace an
//          explicit annotation (the lambda visitor checks `p.type` first).
//   T6     a DIFFERENT parameter type (string T) proves the thread reads the
//          receiver's instantiation rather than defaulting to `int`, which is
//          also what an untyped parameter slot lowers to
//          (`lowerLambda`'s `i32()` default) and would otherwise mask.
//   T7     the §35.4 rule-3 admissibility gate is UNCHANGED: an owning
//          receiver is still rejected, with its own wording, through an
//          un-annotated lambda. The fix supplies a parameter type; it does
//          not widen what is admissible.

#include <string>
#include <string_view>

#include "support/aot_result_asserts.hpp"   // diagnosticMessageContains
#include "support/aot_support.hpp"          // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

// Asserts the program compiled, linked, ran, exited 0, and printed exactly
// `want` (plus the trailing newline `Console.WriteLine` emits). Pinning
// stdout — not the exit code — keeps the oracle away from the values a
// crash or an ASan abort can produce.
void expectRuns(const bezel::test::JitResult& r, std::string_view want) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want) + "\n");
}

// T1 — rule-corpus l2_20_0002, verbatim (stdlib §2.20: "Map(f) maps Ok(v) to
// Ok(f(v))"). Pre-fix: `Result.Map` requires a function-typed argument.
BEZEL_TEST(result_combinator_untyped_lambda_param_2355,
           stdlib_2_20_map_transforms_ok_payload) {
    auto r = runAot(R"(
module Conf;

public class Program
  public static method Main()
    var r: Result<int, string> := Result<int, string>.Ok(5);
    var r2 := r.Map(lambda(x) => x * 2);
    case r2 of
      Ok(var v) => Console.WriteLine(v),
      Error(var e) => Console.WriteLine(e)
    end case;
  end Main;
end Program;
)");
    expectRuns(r, "10");
}

// T2 — rule-corpus l2_20_0003, verbatim (stdlib §2.20: "Map(f) ... passes
// Error(e) through unchanged"). The un-annotated `x` is typed even though
// the Ok arm is never taken.
BEZEL_TEST(result_combinator_untyped_lambda_param_2355,
           stdlib_2_20_map_passes_error_through) {
    auto r = runAot(R"(
module Conf;

public class Program
  public static method Main()
    var r: Result<int, string> := Result<int, string>.Error("bad");
    var r2 := r.Map(lambda(x) => x * 2);
    case r2 of
      Ok(var v) => Console.WriteLine(v),
      Error(var e) => Console.WriteLine(e)
    end case;
  end Main;
end Program;
)");
    expectRuns(r, "bad");
}

// T3 — rule-corpus l2_20_0004, verbatim (stdlib §2.20: "MapError(f) maps
// Error(e) to Error(f(e))"). This row proves the thread picks E, not T: the
// receiver is `Result<int, string>` and the parameter must be `string` for
// `e.Length` to resolve at all.
BEZEL_TEST(result_combinator_untyped_lambda_param_2355,
           stdlib_2_20_maperror_transforms_error_payload) {
    auto r = runAot(R"(
module Conf;

public class Program
  public static method Main()
    var r: Result<int, string> := Result<int, string>.Error("bad");
    var r2 := r.MapError(lambda(e) => e.Length);
    case r2 of
      Ok(var v) => Console.WriteLine(v),
      Error(var e) => Console.WriteLine(e)
    end case;
  end Main;
end Program;
)");
    expectRuns(r, "3");
}

// T4 — stdlib §2.20: "MapError(f) ... passes Ok(v) through". The E-typed
// parameter thread still has to fire on the arm that is not taken, or the
// call does not type-check at all.
BEZEL_TEST(result_combinator_untyped_lambda_param_2355,
           stdlib_2_20_maperror_passes_ok_through) {
    auto r = runAot(R"(
module Conf;

public class Program
  public static method Main()
    var r: Result<int, string> := Result<int, string>.Ok(7);
    var r2 := r.MapError(lambda(e) => e.Length);
    case r2 of
      Ok(var v) => Console.WriteLine(v),
      Error(var e) => Console.WriteLine(e)
    end case;
  end Main;
end Program;
)");
    expectRuns(r, "7");
}

// T5 — the ANNOTATED spelling is untouched. §27.4 makes the annotation
// optional, not forbidden, and the lambda visitor consults `p.type` before
// any inference thread; this row fails if the new thread ever overrides an
// explicit annotation.
BEZEL_TEST(result_combinator_untyped_lambda_param_2355,
           spec_27_4_explicit_annotation_still_wins) {
    auto r = runAot(R"(
module Conf;

public class Program
  public static method Main()
    var r: Result<int, string> := Result<int, string>.Ok(5);
    var r2 := r.Map(lambda(x: int) => x * 2);
    case r2 of
      Ok(var v) => Console.WriteLine(v),
      Error(var e) => Console.WriteLine(e)
    end case;
  end Main;
end Program;
)");
    expectRuns(r, "10");
}

// T6 — the thread reads the RECEIVER's T, not a default. With T = string the
// body calls `x.Length`, which only resolves on a `string`-typed parameter;
// an untyped slot lowers through `lowerLambda`'s `i32()` default, so a
// hard-wired `int` would both mis-resolve here and mis-size the slot.
BEZEL_TEST(result_combinator_untyped_lambda_param_2355,
           stdlib_2_20_map_param_is_receiver_t_not_int) {
    auto r = runAot(R"(
module Conf;

public class Program
  public static method Main()
    var r: Result<string, int> := Result<string, int>.Ok("abcd");
    var r2 := r.Map(lambda(s) => s.Length + 1);
    case r2 of
      Ok(var v) => Console.WriteLine(v),
      Error(var e) => Console.WriteLine(e)
    end case;
  end Main;
end Program;
)");
    expectRuns(r, "5");
}

// T7 — the §35.4 rule-3 admissibility gate is unchanged. A class payload
// makes the receiver instantiation owning (§35.4), and stdlib §2.20 says
// "Owning instantiations carry none of these members". Supplying a parameter
// type must not make the call admissible; the specific value-like wording
// (not merely "some error") is asserted so a crash or an unrelated rejection
// cannot satisfy this row.
BEZEL_TEST(result_combinator_untyped_lambda_param_2355,
           spec_35_4_rule3_owning_receiver_still_rejected) {
    auto r = runAot(R"(
module Conf;

public class Holder
  public constructor()
  end constructor;
end Holder;

public class Program
  public static method Main()
    var r: Result<Holder, string> := Result<Holder, string>.Error("bad");
    var r2 := r.Map(lambda(h) => 1);
    Console.WriteLine("unreachable");
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "only admissible on a value-like"));
}

}  // namespace
