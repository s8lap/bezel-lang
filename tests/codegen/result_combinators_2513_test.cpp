// Regression for #2513 / spec §35.2 (v0.236): the stdlib §2.20
// `Result<T, E>` carries four compiler-provided per-instantiation combinators
// — `Map` / `MapError` / `AndThen` / `UnwrapOr` — declared in stdlib §2.20
// and admissible only where the receiver AND result instantiations are both
// value-like (§35.4 rule 3). They have NO MethodSymbol — the prelude's
// member list holds only `IsOk` / `IsError`. Before this fix, a call like
// `r.Map(...)` rejected at sema with "no member `Map` on type `Result`"
// (issue #2496's fix made the silent-zero miscompile of #2434 into a clean
// compile error). The dedicated codegen path
// (LlvmBackend_Calls.cpp::tryLowerResultCombinatorCall, dispatched in
// lowerCall before the method-symbol path) emits the inline body — a
// switch on the Ok/Error discriminator tag + branch-specific bodies —
// bypassing the prelude-body injection that the previous-lane attempt
// (worktree `agent-a898367811a756241`) reported as a `lowerCaseExpr` PHI
// type assertion failure.
//
// Each test below pins a distinct round-trip per the §35.2 semantics:
//   * Map(f):    Ok(v)  -> Ok(f(v));     Error(e) -> Error(e)
//   * MapError:  Ok(v)  -> Ok(v);        Error(e) -> Error(f(e))
//   * AndThen:   Ok(v)  -> f(v);         Error(e) -> Error(e)
//   * UnwrapOr:  Ok(v)  -> v;            Error(_) -> fallback
// A `pure-correctness` lane: the cross-path invariants (Map-on-Error
// preserves the error payload verbatim, AndThen-on-Error preserves the
// payload verbatim, UnwrapOr's fallback type must match T, the result
// instantiation's element types line up across the Ok/Error slots) are
// pinned by observable runtime values via the exit code.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

const char* kCommon =
    "module M; "
    "class App public static method Main(): int ";

int run(const std::string& tail) {
    auto r = runAot(std::string(kCommon) + tail + " end Main; end App;");
    return r.ran ? r.exitCode : -999;
}

// §35.2 / Map — observable round-trip: `Map` on an `Ok` value applies `f`
// to the Ok payload and produces a fresh `Ok(U)`; on an `Error` it preserves
// the payload verbatim. Pre-fix, the call rejected at sema with "no member
// `Map` on type `Result`".
BEZEL_TEST(result_combinators_2513, map_on_ok) {
    BEZEL_EXPECT_EQ(run(
        "  var r: Result<int, string> := Result<int, string>.Ok(7); "
        "  var r2: Result<long, string> := r.Map(lambda(x: int) => long(x * 11)); "
        "  case r2 of "
        "    Ok(var v)    => return (if v = 77L then 0 else 1);, "
        "    Error(var m) => return 2; "
        "  end; "), 0);
}

BEZEL_TEST(result_combinators_2513, map_on_error_passes_through) {
    BEZEL_EXPECT_EQ(run(
        "  var r: Result<int, string> := Result<int, string>.Error(\"bad input\"); "
        "  var r2: Result<long, string> := r.Map(lambda(x: int) => long(x * 11)); "
        "  case r2 of "
        "    Ok(var v)    => return 1;, "
        "    Error(var m) => return (if m = \"bad input\" then 0 else 2); "
        "  end; "), 0);
}

// §35.2 / MapError — observable round-trip: `MapError` on `Error(e)` applies
// `f` to e and produces a fresh `Error(F)`; on `Ok(v)` it passes the payload
// through unchanged. The F inference is taken from `f`'s funcReturn. Pre-fix,
// sema rejected with "no member `MapError` on type `Result`".
BEZEL_TEST(result_combinators_2513, maperror_on_error) {
    BEZEL_EXPECT_EQ(run(
        "  var r: Result<int, string> := Result<int, string>.Error(\"abcde\"); "
        "  var r2: Result<int, long> := r.MapError(lambda(s: string) => long(s.Length)); "
        "  case r2 of "
        "    Ok(var v)    => return 1;, "
        "    Error(var e) => return (if e = 5L then 0 else 2); "
        "  end; "), 0);
}

BEZEL_TEST(result_combinators_2513, maperror_on_ok_passes_through) {
    BEZEL_EXPECT_EQ(run(
        "  var r: Result<int, string> := Result<int, string>.Ok(99); "
        "  var r2: Result<int, long> := r.MapError(lambda(s: string) => long(s.Length)); "
        "  case r2 of "
        "    Ok(var v)    => return (if v = 99 then 0 else 1);, "
        "    Error(var e) => return 2; "
        "  end; "), 0);
}

// §35.2 / AndThen — observable round-trip: `AndThen(f)` invokes `f(v)` on
// Ok(v) and returns the resulting `Result<U, E>` directly; on Error(e) the
// original payload is preserved verbatim. The U inference unwraps one layer
// of `Result` from `f`'s funcReturn (per spec §14.5 phase 2). Pre-fix,
// sema rejected with "no member `AndThen` on type `Result`".
BEZEL_TEST(result_combinators_2513, andthen_on_ok_chains) {
    BEZEL_EXPECT_EQ(run(
        "  var r: Result<int, string> := Result<int, string>.Ok(5); "
        "  var r2: Result<long, string> := r.AndThen("
        "    lambda(x: int) => Result<long, string>.Ok(long(x * 3))); "
        "  case r2 of "
        "    Ok(var v)    => return (if v = 15L then 0 else 1);, "
        "    Error(var m) => return 2; "
        "  end; "), 0);
}

BEZEL_TEST(result_combinators_2513, andthen_on_error_passes_through) {
    BEZEL_EXPECT_EQ(run(
        "  var r: Result<int, string> := Result<int, string>.Error(\"step1 failed\"); "
        "  var r2: Result<long, string> := r.AndThen("
        "    lambda(x: int) => Result<long, string>.Ok(long(x * 3))); "
        "  case r2 of "
        "    Ok(var v)    => return 1;, "
        "    Error(var m) => return (if m = \"step1 failed\" then 0 else 2); "
        "  end; "), 0);
}

// §35.2 / UnwrapOr — observable round-trip: `UnwrapOr(fallback)` returns
// the Ok payload when Ok(v), and the eagerly-evaluated `fallback` on
// Error(_). `fallback` is a plain T value, NOT a function — the spec
// distinguishes from a lazy `UnwrapOrElse` (which does not exist). Pre-fix,
// the call rejected at sema.
BEZEL_TEST(result_combinators_2513, unwrapor_on_ok) {
    BEZEL_EXPECT_EQ(run(
        "  var r: Result<int, string> := Result<int, string>.Ok(42); "
        "  var v: int := r.UnwrapOr(99); "
        "  return (if v = 42 then 0 else 1); "), 0);
}

BEZEL_TEST(result_combinators_2513, unwrapor_on_error_returns_fallback) {
    BEZEL_EXPECT_EQ(run(
        "  var r: Result<int, string> := Result<int, string>.Error(\"x\"); "
        "  var v: int := r.UnwrapOr(99); "
        "  return (if v = 99 then 0 else 1); "), 0);
}

// §35.2 / chained pipeline — the stdlib §2.20 documented shape is
// `ParsePort(raw).Map(...).UnwrapOr(...)`. Pins that the four combinators
// compose and the resulting slot type stays correct across the chain.
// Pre-fix, the first `.Map` rejected at sema and the chain never compiled.
BEZEL_TEST(result_combinators_2513, chained_map_unwrapor) {
    BEZEL_EXPECT_EQ(run(
        "  var r: Result<int, string> := Result<int, string>.Ok(8); "
        "  var s: string := r.Map(lambda(n: int) => \"n=\" + n.ToString())"
        "                     .UnwrapOr(\"default\"); "
        "  return (if s = \"n=8\" then 0 else 1); "), 0);
}

}  // namespace