// Spec-derived tests for the v0.179 async-borrow soundness rejections
// (§30.1 "Async methods", §29.4 "Lazy evaluation"; issues #1438/#1518 and
// #1448/#1519).
//
//   #1518 (§30.1, "Borrow-bearing parameters and results are forbidden"):
//     an `async` method / async iterator may not declare a parameter, a
//     result, or an async-iterator element type that is a borrow-bearing
//     record (§9.7) — a span (stdlib §4.14/§4.15) or any record carrying a
//     non-runtime-managed borrow. An async method captures its parameters
//     into a heap state-machine frame that outlives the synchronous call, so
//     a captured borrow has no stack frame whose unwinding bounds it. The one
//     exception is `CancellationToken` (§30.8), the single stdlib-certified
//     record; records whose only borrows are runtime-managed (`string`,
//     function types) are not borrow-bearing (§9.7) and are unaffected.
//
//   #1519 (§30.1 "Borrow-bearing record locals across suspension" / §29.4):
//     a local of borrow-bearing-record type whose live range crosses a
//     suspension point (`await`, `await using`, `await for`, or `yield return`
//     in an iterator — sync or async) is rejected unless every borrow it
//     carries has a frame-resident source: an owned local, a `using var`, or
//     an `owned` parameter of the same body (locations the frame owns and that
//     therefore survive every suspension). A caller-side source (a plain
//     parameter, `this`, a field, or an enclosing capture) may be disposed
//     while the frame is suspended and is rejected. A local whose live range
//     does NOT cross a suspension is unaffected.
//
// // per §30.1, §29.4 (v0.179)

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// True when an ERROR diagnostic whose message contains `sub` was emitted.
// Matching on the specific §30.1/§29.4 wording keeps each test pinned to the
// rule under test (an unrelated diagnostic never satisfies it).
bool hasError(const OwnershipResult& r, const std::string& sub) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(sub) != std::string::npos) return true;
    }
    return false;
}

// The distinctive substrings of the three diagnostics under test.
const char* kParamMsg = "may not be a parameter of an `async`";
const char* kResultMsg = "borrow-bearing result or element type";
const char* kSuspendMsg = "is held live across a suspension point";

// ===========================================================================
// #1518 — borrow-bearing record params / results forbidden on async (§30.1).
// ===========================================================================

// A borrowing span (`ArraySpan<T>`, a view over an `Array<T>`, stdlib §4.15)
// as an `async` parameter is rejected. // per §30.1
BEZEL_TEST(async_borrow_1518, param_arrayspan_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public async method Work(s: ArraySpan<byte>): Task "
        "    var z := s.Length; "
        "  end Work; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kParamMsg));
}

// An owning span (`Span<T>`, a view over an `OwningBuffer<T>`, stdlib §4.14)
// is borrow-bearing too (it carries a borrow of the buffer) — rejected as an
// async parameter. // per §30.1
BEZEL_TEST(async_borrow_1518, param_span_over_buffer_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public async method Work(s: Span<byte>): Task "
        "    var z := s.Length; "
        "  end Work; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kParamMsg));
}

// A user record carrying a non-runtime-managed borrow (a class-typed field) is
// a borrow-bearing record (§9.7) and is rejected as an async parameter. // per §30.1
BEZEL_TEST(async_borrow_1518, param_user_borrow_bearing_record_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class Res end Res; record Holder(R: Res); class C "
        "  public async method Work(h: Holder): Task "
        "    var z := 1; "
        "  end Work; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kParamMsg));
}

// A borrow-bearing span as the async RESULT type (`Task<ArraySpan<T>>`) is
// rejected — the result would escape the frame with no bounded source. // per §30.1
BEZEL_TEST(async_borrow_1518, result_span_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public async method Work(arr: Array<byte>): Task<ArraySpan<byte>> "
        "    return arr.AsSpan(); "
        "  end Work; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kResultMsg));
}

// `CancellationToken` (§30.8) is the single stdlib-certified exception: a
// borrow-bearing record permitted as an async parameter. // per §30.1, §30.8
BEZEL_TEST(async_borrow_1518, param_cancellation_token_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public async method Work(ct: CancellationToken): Task "
        "    var z := 1; "
        "  end Work; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasError(r, kParamMsg));
}

// A record whose only reference-typed field is runtime-managed (`string`) is
// NOT borrow-bearing (§9.7) — admitted as an async parameter. // per §30.1, §9.7
BEZEL_TEST(async_borrow_1518, param_runtime_managed_record_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; record Label(Text: string); class C "
        "  public async method Work(l: Label): Task "
        "    var z := 1; "
        "  end Work; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasError(r, kParamMsg));
}

// The prohibition is async-only: a synchronous method may take a span
// parameter (its borrow is bounded by the ordinary call frame, §28.3). // per §30.1
BEZEL_TEST(async_borrow_1518, nonasync_span_param_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method Work(s: ArraySpan<byte>): int "
        "    return s.Length; "
        "  end Work; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasError(r, kParamMsg));
}

// ===========================================================================
// #1519 — borrow-bearing record local across a suspension point (§30.1/§29.4).
// ===========================================================================

// A span local derived from a plain (caller-side) parameter and read after an
// `await` crosses the suspension with a non-frame-resident source — rejected.
// This is the exact hole the issue names. // per §30.1
BEZEL_TEST(async_borrow_1519, local_span_across_await_caller_source_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method Wait(): Task => Task.Delay(0); "
        "  public async method Work(arr: Array<byte>): Task "
        "    var s := arr.AsSpan(); "
        "    await Wait(); "
        "    var z := s.Length; "
        "  end Work; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kSuspendMsg));
}

// A span local whose source is a `using var` — a frame-resident owner — may
// cross the `await`: the frame owns the source, which survives the
// suspension. // per §30.1
BEZEL_TEST(async_borrow_1519, local_span_using_var_source_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method Wait(): Task => Task.Delay(0); "
        "  public async method Work(): Task "
        "    using var arr := new Array<byte>(); "
        "    var s := arr.AsSpan(); "
        "    await Wait(); "
        "    var z := s.Length; "
        "  end Work; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasError(r, kSuspendMsg));
}

// A span local whose source is an `owned` parameter — transferred into the
// frame at call time (§30.1 "`owned` parameters transfer into the frame") —
// may cross the `await`. // per §30.1
BEZEL_TEST(async_borrow_1519, local_span_owned_param_source_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method Wait(): Task => Task.Delay(0); "
        "  public async method Work(owned arr: Array<byte>): Task "
        "    var s := arr.AsSpan(); "
        "    await Wait(); "
        "    var z := s.Length; "
        "  end Work; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasError(r, kSuspendMsg));
}

// A span local whose entire live range precedes the `await` (last use before
// the suspension) does not cross it — admitted. Guards that the check keys on
// liveness across the suspension, not merely on declaration order. // per §30.1
BEZEL_TEST(async_borrow_1519, local_span_used_before_await_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method Wait(): Task => Task.Delay(0); "
        "  public async method Work(arr: Array<byte>): Task "
        "    var s := arr.AsSpan(); "
        "    var n := s.Length; "
        "    await Wait(); "
        "    var z := n; "
        "  end Work; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasError(r, kSuspendMsg));
}

// A span local DECLARED after the `await` never crosses a suspension —
// admitted. // per §30.1
BEZEL_TEST(async_borrow_1519, local_span_declared_after_await_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method Wait(): Task => Task.Delay(0); "
        "  public async method Work(arr: Array<byte>): Task "
        "    await Wait(); "
        "    var s := arr.AsSpan(); "
        "    var z := s.Length; "
        "  end Work; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasError(r, kSuspendMsg));
}

// The rule extends to a SYNC iterator's `yield return` (§29.4): its heap frame
// has the same shape, and the consumer may dispose caller-side sources between
// `MoveNext` calls. A span local from a caller-side source read after a
// `yield return` is rejected. // per §29.4, §30.1
BEZEL_TEST(async_borrow_1519, sync_iterator_span_across_yield_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method Gen(arr: Array<byte>): IEnumerable<int> "
        "    var s := arr.AsSpan(); "
        "    yield return 1; "
        "    yield return s.Length; "
        "  end Gen; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kSuspendMsg));
}

// A sync iterator's span local used entirely before the first `yield return`
// does not cross it — admitted. // per §29.4
BEZEL_TEST(async_borrow_1519, sync_iterator_span_used_before_yield_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method Gen(arr: Array<byte>): IEnumerable<int> "
        "    var s := arr.AsSpan(); "
        "    var n := s.Length; "
        "    yield return n; "
        "    yield return 2; "
        "  end Gen; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasError(r, kSuspendMsg));
}

}  // namespace
