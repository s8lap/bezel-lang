// Spec-derived sema tests for fix #1670 — §30.1 "async methods" / §29.3
// "Iterator methods" / §30.6 "Async lambdas" (v0.184):
//
//   §30.1: "`ref` and `out` parameters are forbidden (v0.184). An `async`
//   method, `async` lambda, or iterator method — synchronous or async
//   (§29.3) — may not declare a `ref` or `out` parameter; a compile error at
//   the declaration. ... The rule extends to conversions: an `async` lambda
//   may not be assigned to a function type (§27.1) whose parameter list
//   carries a `ref`/`out` slot. Non-`async` methods that merely *return*
//   `Task`-family types are unaffected (no state machine — the out-contract
//   completes synchronously), as are `owned` parameters (§29.8, above)."
//
//   §29.3: "An iterator method may not declare `ref` or `out` parameters
//   (v0.184): its arguments are captured into a heap state-machine frame
//   that outlives the synchronous call, and a caller-stack-slot reference
//   cannot be — the same rule and reasoning as §30.1's, stated there.
//   `owned` parameters remain admitted (§29.8)."
//
//   §30.6: "The parameter slots of an async lambda's function type are
//   plain, `owned`, or `scoped` only: a target type carrying a `ref` or
//   `out` slot is rejected for an `async` lambda (§30.1, v0.184)."
//
// Before this fix the compiler accepted `ref`/`out` on all three forms (an
// async method, a sync/async iterator method, and an async lambda's own
// parameter list) — the arguments are captured into a heap-allocated
// state-machine frame that outlives the synchronous call, so a caller-
// stack-slot reference dangles the moment the method/lambda returns its
// task or enumerable at the first suspension point. This file pins the
// three declaration-site rejections plus positive controls (ordinary
// ref/out methods, `owned` params on async/iterator, and non-async lambda
// ref params — all unaffected by the rule).
//
// Authority: §30.1, §29.3, §30.6 (v0.184, #1670).

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// The distinctive substrings of the three §1670 diagnostics.
const char* kAsyncMethodMsg = "is not permitted on an `async` method";
const char* kIteratorMsg = "is not permitted on an iterator method";
const char* kAsyncLambdaMsg = "is not permitted on an `async` lambda";

bool hasError(const bezel::test::SemaResult& r, const std::string& sub) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(sub) != std::string::npos) return true;
    }
    return false;
}

// ===========================================================================
// §30.1 — `ref`/`out` on an `async` method: rejected.
// ===========================================================================

// A `ref` parameter on an `async` method — rejected. Fails without the fix
// (the compiler previously accepted this declaration outright). // per §30.1
BEZEL_TEST(async_iterator_refout_1670, async_method_ref_rejected) {
    auto r = parseSemaAll(
        "module M; class Foo "
        "  public async method Bump(ref x: int): Task "
        "    x := x + 1; return; "
        "  end Bump; "
        "end Foo;");
    BEZEL_EXPECT_TRUE(hasError(r, kAsyncMethodMsg));
}

// An `out` parameter on an `async` method — rejected. // per §30.1
BEZEL_TEST(async_iterator_refout_1670, async_method_out_rejected) {
    auto r = parseSemaAll(
        "module M; class Foo "
        "  public async method Fill(out x: int): Task "
        "    x := 5; return; "
        "  end Fill; "
        "end Foo;");
    BEZEL_EXPECT_TRUE(hasError(r, kAsyncMethodMsg));
}

// ===========================================================================
// §29.3 — `ref`/`out` on an iterator method (synchronous or async):
// rejected.
// ===========================================================================

// A `ref` parameter on a SYNCHRONOUS iterator method (`yield return` makes
// it an iterator, §29.3) — rejected. // per §29.3
BEZEL_TEST(async_iterator_refout_1670, sync_iterator_ref_rejected) {
    auto r = parseSemaAll(
        "module M; class Foo "
        "  public method Gen(ref n: int): IEnumerable<int> "
        "    n := n + 1; yield return n; "
        "  end Gen; "
        "end Foo;");
    BEZEL_EXPECT_TRUE(hasError(r, kIteratorMsg));
}

// An `out` parameter on a synchronous iterator method — rejected. // per §29.3
BEZEL_TEST(async_iterator_refout_1670, sync_iterator_out_rejected) {
    auto r = parseSemaAll(
        "module M; class Foo "
        "  public method Gen(out n: int): IEnumerable<int> "
        "    n := 3; yield return n; "
        "  end Gen; "
        "end Foo;");
    BEZEL_EXPECT_TRUE(hasError(r, kIteratorMsg));
}

// An ASYNC iterator (`async` + `yield return`, §30.7) with a `ref` parameter
// is rejected too — both §30.1 and §29.3 independently condemn it; the
// async leg of the check fires first. // per §30.1, §29.3, §30.7
BEZEL_TEST(async_iterator_refout_1670, async_iterator_ref_rejected) {
    auto r = parseSemaAll(
        "module M; class Foo "
        "  public async method Gen(ref n: int): IAsyncEnumerable<int> "
        "    n := n + 1; yield return n; "
        "  end Gen; "
        "end Foo;");
    BEZEL_EXPECT_TRUE(hasError(r, kAsyncMethodMsg));
}

// ===========================================================================
// §30.6 — `ref`/`out` on an async lambda's own parameter list: rejected.
// ===========================================================================

// An async lambda declaring an explicit `ref` parameter in its own param
// list — rejected, even though the local `f` never escapes the enclosing
// method (the frame-capture hazard is unconditional, not escape-dependent).
// // per §30.6
BEZEL_TEST(async_iterator_refout_1670, async_lambda_ref_rejected) {
    auto r = parseSemaAll(
        "module M; class Foo "
        "  public static async method Run(): Task "
        "    var f: (ref int) -> Task := async (ref x: int) => do "
        "      x := x + 1; return; "
        "    end; "
        "    return; "
        "  end Run; "
        "end Foo;");
    BEZEL_EXPECT_TRUE(hasError(r, kAsyncLambdaMsg));
}

// An async lambda declaring an explicit `out` parameter — rejected. // per §30.6
BEZEL_TEST(async_iterator_refout_1670, async_lambda_out_rejected) {
    auto r = parseSemaAll(
        "module M; class Foo "
        "  public static async method Run(): Task "
        "    var f: (out int) -> Task := async (out x: int) => do "
        "      x := 5; return; "
        "    end; "
        "    return; "
        "  end Run; "
        "end Foo;");
    BEZEL_EXPECT_TRUE(hasError(r, kAsyncLambdaMsg));
}

// ===========================================================================
// Positive controls — forms the rule does NOT touch stay accepted.
// ===========================================================================

// An ORDINARY (non-async, non-iterator) method with a `ref` parameter is
// unaffected — its by-reference argument is bounded by the ordinary
// synchronous call frame, no heap state machine involved. // per §30.1, §22.3
BEZEL_TEST(async_iterator_refout_1670, ordinary_method_ref_accepted) {
    auto r = parseSemaAll(
        "module M; class Foo "
        "  public method Bump(ref x: int) "
        "    x := x + 1; "
        "  end Bump; "
        "end Foo;");
    BEZEL_EXPECT_FALSE(hasError(r, kAsyncMethodMsg));
    BEZEL_EXPECT_FALSE(hasError(r, kIteratorMsg));
}

// An ordinary method with an `out` parameter is unaffected. // per §30.1, §22.3
BEZEL_TEST(async_iterator_refout_1670, ordinary_method_out_accepted) {
    auto r = parseSemaAll(
        "module M; class Foo "
        "  public method Fill(out x: int) "
        "    x := 5; "
        "  end Fill; "
        "end Foo;");
    BEZEL_EXPECT_FALSE(hasError(r, kAsyncMethodMsg));
    BEZEL_EXPECT_FALSE(hasError(r, kIteratorMsg));
}

// `owned` parameters are explicitly exempted by both §30.1 and §29.3 — an
// `async` method taking an `owned` parameter is unaffected. // per §30.1, §29.8
BEZEL_TEST(async_iterator_refout_1670, async_method_owned_param_accepted) {
    auto r = parseSemaAll(
        "module M; class Res destructor end destructor; end Res; "
        "class Foo "
        "  public async method Take(owned r: Res): Task "
        "    using var x := r; return; "
        "  end Take; "
        "end Foo;");
    BEZEL_EXPECT_FALSE(hasError(r, kAsyncMethodMsg));
}

// `owned` parameters on an iterator method are likewise unaffected. // per §29.3, §29.8
BEZEL_TEST(async_iterator_refout_1670, iterator_owned_param_accepted) {
    auto r = parseSemaAll(
        "module M; class Res destructor end destructor; end Res; "
        "class Foo "
        "  public method Gen(owned r: Res): IEnumerable<int> "
        "    using var x := r; yield return 1; "
        "  end Gen; "
        "end Foo;");
    BEZEL_EXPECT_FALSE(hasError(r, kIteratorMsg));
}

// A SYNCHRONOUS (non-`async`) lambda with an explicit `ref` parameter is
// unaffected — §30.6's rejection is async-lambda-only; a plain lambda's
// parameters are bounded by the ordinary call, no state machine. // per §30.6
BEZEL_TEST(async_iterator_refout_1670, sync_lambda_ref_accepted) {
    auto r = parseSemaAll(
        "module M; class Foo "
        "  public static method Run() "
        "    var f: (ref int) -> () := (ref x: int) => do "
        "      x := x + 1; "
        "    end; "
        "    return; "
        "  end Run; "
        "end Foo;");
    BEZEL_EXPECT_FALSE(hasError(r, kAsyncLambdaMsg));
}

// Sanity: an `async` method / iterator method with NO `ref`/`out` parameter
// is unaffected by the rule (no false positive on an ordinary plain
// parameter). // per §30.1, §29.3
BEZEL_TEST(async_iterator_refout_1670, plain_async_and_iterator_no_refout_accepted) {
    auto rAsync = parseSemaAll(
        "module M; class Foo "
        "  public async method Bump(x: int): Task<int> "
        "    return x + 1; "
        "  end Bump; "
        "end Foo;");
    BEZEL_EXPECT_FALSE(hasError(rAsync, kAsyncMethodMsg));

    auto rIter = parseSemaAll(
        "module M; class Foo "
        "  public method Gen(n: int): IEnumerable<int> "
        "    for var i in 0..<n do yield return i; end; "
        "  end Gen; "
        "end Foo;");
    BEZEL_EXPECT_FALSE(hasError(rIter, kIteratorMsg));
}

}  // namespace
