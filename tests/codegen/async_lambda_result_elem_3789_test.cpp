// Codegen — issue #3789 (§30.6 async lambdas, spec v0.287): `lowerLambda` passes
// `emitAsyncLambda` an argument named `resultElemSemaTy` (§30.6's T, the result
// element type of `Task<T>` / `OwningTask<T>`), but on the NO call-site-
// annotation path it passed the WHOLE wrapped `Task<T>` instead of T. The two
// derivations disagreed silently and any consumer that asked a question about T
// got a `Kind::Named` task type for one of them.
//
// WHY THE FALLBACK PASSED THE WRAPPER. The no-annotation path was meant to use
// the body's natural result type as T, but #800 Layer-2 explicitly rewrote
// `retSemaTy` for an async expression body to read `lamTy->funcReturn` — i.e.
// the WRAPPED `Task<T>` ("Derive retTy/retSemaTy from the wrapped funcReturn
// (Task<T>) ... NOT from the raw body element type"). The pre-fix fallback then
// handed the wrapper straight through, where the call-site-annotation path
// correctly extracted T via `genericArgs[0]`. Mirroring the annotation path
// extracts T uniformly.
//
// HOW THE BUG STAYED SILENT FOR A LONG TIME. `emitAsyncLambda` stores
// `resultElemSemaTy` in `asyncResultSemaType_` and derives `asyncResultTy_ =
// lowerTypeRef(resultElemSemaTy)` — and for closure-typed T, both
// `lowerTypeRef(Task<() -> int>)` and `lowerTypeRef(() -> int)` are `ptr`, so
// the two paths produced the same size by coincidence (#3773 noted this).
// The same coincidence saved the `int` element shape at small values: a 4-byte
// `i32` was memcpy'd as 8 bytes from a stack slot, and the 4-byte consumer
// read the low 4 — which happened to be the value. For element T where
// `sizeof(T) > sizeof(ptr)` (e.g. a 20-byte record), the consumer reads past
// the end of an 8-byte buffer into garbage, and the wrong-value / SIGBUS
// failure mode appears.
//
// THE FIX mirrors the call-site-annotation path exactly:
//   if (!retSemaTy->genericArgs.empty() && retSemaTy->genericArgs[0])
//       asyncResultElemSemaTy = retSemaTy->genericArgs[0];
// so both paths now produce the same T, and `asyncResultTy_` sizes
// `bz_task_complete`'s memcpy correctly. The element type argument is now
// load-bearing for any non-pointer element T — exactly the shape §30.6 names.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// fix #3789, Row 1 — bug repro: an async lambda with NO call-site annotation,
// returning a record T where `sizeof(T) > sizeof(ptr)`. Pre-fix:
// `asyncResultTy_ = ptrTy()` (8 bytes), so `bz_task_complete` memcpy's 8 bytes
// for a 20-byte record — the consumer reads 8 bytes for the lower fields and
// returns 3 (1 + 2). Post-fix: `asyncResultTy_ = the record's LLVM struct type`
// (20 bytes), all 5 fields round-trip, returns 15 (1+2+3+4+5). The `int` shape
// named in the brief (the trivial `async () => 42`) is silent both pre- and
// post-fix on this codepath (a 4-byte value memcpy'd as 8 still reads back
// correctly via the consumer's 4-byte load), so the record shape is what makes
// the regression test fail without the fix.
BEZEL_TEST(async_lambda_result_elem_3789, no_annotation_record_element) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public record R5 "
        "    public a: int; public b: int; public c: int; "
        "    public d: int; public e: int; "
        "  end R5; "
        "  public static method Main(): int "
        "    var task := async lambda() => new R5(a := 1, b := 2, c := 3, d := 4, e := 5); "
        "    var r := task().Result; "
        "    return r.a + r.b + r.c + r.d + r.e; "
        "  end Main; end C;"),
        15);
}

// fix #3789, Row 2 — control (NO regression): an async lambda WITH an explicit
// `() -> Task<int>` call-site annotation. The annotation path already extracted
// T correctly via `genericArgs[0]` before this fix; this row guards against
// the fix breaking that path. Returns 42 in both pre- and post-fix.
BEZEL_TEST(async_lambda_result_elem_3789, with_annotation_int_element) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Main(): int "
        "    var task: () -> Task<int> := async lambda() => 42; "
        "    return task().Result; "
        "  end Main; end C;"),
        42);
}

// fix #3789, Row 3 — control (NO regression): an async lambda with no call-site
// annotation returning a CAPTURED value. The captured-variable path goes
// through the same `#800 Layer-2 retSemaTy = funcReturn` rewrite but with
// `x`'s cell-type's own sema flow; both pre- and post-fix return 5.
BEZEL_TEST(async_lambda_result_elem_3789, no_annotation_captured_int) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Main(): int "
        "    var x := 5; "
        "    var task := async lambda() => x; "
        "    return task().Result; "
        "  end Main; end C;"),
        5);
}

// fix #3789, Row 4 — control (NO regression): a SYNCHRONOUS lambda (no `async`
// keyword). The async branch of `lowerLambda` is bypassed entirely; the
// `resultElemSemaTy` derivation is not consulted. Returns 42.
BEZEL_TEST(async_lambda_result_elem_3789, sync_lambda_unchanged) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Main(): int "
        "    var task := lambda() => 42; "
        "    return task(); "
        "  end Main; end C;"),
        42);
}