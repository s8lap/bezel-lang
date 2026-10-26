// Codegen — issue #2361: an exception thrown out of a GENERIC method call
// double-freed the caller's own borrowing `Array<T>` local (and any other
// owned local live in the caller's scope at the call), crashing inside
// `bz_array_free` on stack unwind.
//
// THE BUG. `Emitter::emitInlineGenericMethod` (LlvmBackend_Calls2.cpp) emits a
// generic method call's specialized body INTO THE CALLER'S OWN FUNCTION — it
// shares the caller's `disposeStack_` (never clears or saves/restores it; see
// this function's own #628 comment: "the inlining happens inside the CALLER's
// function"). But it unconditionally reset `unwindScopeDepth_ = 0` before
// delegating to the block-body path (`emitGenericSpecialization`, which just
// calls the already-compiled specialized function and then runs the ordinary
// post-call `emitPendingCheck()` — functionally identical to any other direct
// call's post-call check). With `unwindScopeDepth_` wrongly at 0,
// `emitUnwindDisposals()` (invoked by that post-call pending-check) walked
// the ENTIRE shared `disposeStack_` — every owned local already live in the
// CALLER's scope, not just anything belonging to the inlined call — and
// disposed them immediately, right at the call site. Those SAME locals are
// then disposed AGAIN by the enclosing `try`'s own shared catch-arm-entry
// disposal (or, on plain-fall-through unwind, by the caller's scope-end
// disposal) when the exception is actually handled — a double free.
//
// §28.3 (v0.234) "Exception-path discharge": "Owned locals ... that are live
// when an exception propagates out of their enclosing block are disposed
// implicitly during stack unwinding. The compiler treats every owned local
// ... as if it were wrapped in an implicit `try`/`finally` whose `finally`
// disposes the value if it is still owned (i.e., not already moved-from and
// not already explicitly disposed)." — i.e. EXACTLY ONCE per owned local on
// the unwind edge. The bug fired that implicit finally twice for the same
// locals. `xs: Array<Meters>` itself is a *borrowing* collection — its
// elements are borrows, never disposed through it (§28.11 "collections do
// not own elements") — and is itself `trivial-dispose` (§28.9 "Runtime-
// managed types": "Backing storage is reclaimed with the instance by the
// implementation-defined mechanism"), so the double free lands on the
// array's own header/backing-store reclaim (`bz_array_free`), not on any
// element.
//
// MINIMIZATION (recorded per CLAUDE.md rule 3 / the issue's own open
// question): confirmed generic-call-specific — the byte-identical non-
// generic control (same program with `Check<T>` written as a concrete
// `Check(items: Array<Meters>)`) does NOT crash; the emitted IR shows the
// non-generic call site's `call.unwind` block is a bare
// `br label %catch.dispatch` (no disposal — correct, defers to the shared
// catch-arm disposal), while the (pre-fix) GENERIC call site's
// `call.unwind` additionally called `bz_array_free`/each `using var`'s
// `Dispose()` inline before branching to the SAME `catch.dispatch` — the
// double free. `using var` on `xs` is not required to reproduce — a plain
// `var xs` (§28.3 auto-drop) hits the same path. Also confirmed the crash
// requires a `try`/`catch` in the SAME frame that owns the array: letting
// the exception propagate all the way out of `Main` uncaught does NOT crash
// (the erroneous early disposal fires once, at the call site, and nothing
// ever disposes those locals a second time on that path — harmless in
// isolation, but still the wrong place per §28.3, which is why the FIX
// addresses the root state bug rather than only the crash).
//
// THE FIX. `emitInlineGenericMethod` now sets
// `unwindScopeDepth_ = disposeStack_.size()` instead of `= 0` — the same
// "nothing already on the shared stack is mine" boundary `lowerTry` uses
// when entering a `try` body (LlvmBackend_Stmts.cpp) — so a pending-check
// reached while calling the specialized body only ever disposes entries the
// inlined call itself pushed (none, for the common case), leaving the
// caller's own already-live locals to the caller's own (single) disposal
// path. This mirrors `buildSpecializedMethodFn`, which pairs
// `unwindScopeDepth_ = 0` with `disposeStack_.clear()` — 0 is the right
// boundary THERE because that path compiles a genuinely fresh function with
// its own empty dispose stack; `emitInlineGenericMethod` never clears
// `disposeStack_`, so 0 was inconsistent with the (unchanged) shared stack.
//
// `runAotChecked` (AOT + ASan/LSan) is required: this is a double-free, which
// a plain exit-code/leak check would not distinguish from a leak or a correct
// run — ASan's SEGV-on-corrupted-free is the oracle that actually pins the
// bug (matches the issue's own reported `bz_array_free` SEGV under
// `--sanitize=address`).

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"  // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// Per-instance destructor marker (mirrors #1425's harness idiom): Id is a
// monotonic counter distinct from Value, so "exactly once" can be checked
// unambiguously even if two instances share a Value.
const char* kMetersClass =
    "public class Meters "
    "  public Value: int; "
    "  public Id: int; "
    "  private static _next: int := 0; "
    "  public constructor(v: int) "
    "    Value := v; Id := _next; _next := _next + 1; "
    "  end constructor; "
    "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
    "end Meters; ";

// The issue's own repro shape (module M / static class F / method-level
// generic `Check<T>(items: Array<T>)` that throws mid-iteration), with the
// destructor-marker Meters class swapped in so exactly-once disposal can be
// verified per instance, not merely inferred from leak-freedom.
std::string program(bool doThrow) {
    std::string body = doThrow
        ? "n := n + 1; if n = 3 then throw new InvalidOperationException(\"boom\"); end; "
        : "n := n + 1; ";
    return std::string("module M; ") + kMetersClass +
        "static class F "
        "  public static method Check<T>(items: Array<T>): int "
        "    var n := 0; "
        "    for var x in items do " + body +
        "    end; "
        "    return n; "
        "  end Check; "
        "end F; "
        "class C public static method Main(): int "
        "  using var m0 := new Meters(10); using var m1 := new Meters(50); "
        "  using var m2 := new Meters(60); "
        "  using var xs := new Array<Meters>(); "
        "  xs.Add(m0); xs.Add(m1); xs.Add(m2); "
        "  try "
        "    var r := F.Check(xs); "
        "    return r; "
        "  catch var e: InvalidOperationException => do "
        "    Console.WriteLine(\"caught\"); "
        "    return 7; "
        "  end; "
        "  end; "
        "end Main; end C;";
}

}  // namespace

// §28.3 (v0.234) Exception-path discharge / §28.9 / §28.11 (#2361): an
// exception thrown out of a generic method call must dispose each of the
// caller's live owned locals (including the borrowing `Array<T>` itself,
// whose own backing store is trivial-dispose) EXACTLY ONCE on the unwind
// edge that reaches the enclosing `catch`. WITHOUT the fix: ASan reports a
// SEGV inside `bz_array_free` (a double free of `xs`'s header/data) ->
// `r.ran` false — matching the issue's own reported crash signature exactly.
BEZEL_TEST(generic_call_exception_unwind_array_double_free_2361,
           thrown_from_generic_call_disposes_caller_locals_exactly_once) {
    JitResult r = runAotChecked(program(/*doThrow=*/true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("caught") != std::string::npos);
    // m0 = id0, m1 = id1, m2 = id2 — each disposed exactly once (xs itself
    // is a borrowing collection, §28.11, and has no destructor to observe;
    // ASan's absence of a SEGV/double-free abort is what proves ITS
    // backing store was freed exactly once).
    for (int id = 0; id <= 2; ++id) {
        std::string marker = "D" + std::to_string(id);
        BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, marker), 1);
    }
}

// Control: the SAME program with the `throw` removed must be entirely
// unaffected by the fix — the normal-exit path never touches
// `emitPendingCheck`'s unwind branch, so `unwindScopeDepth_`'s value is
// moot there; this pins that the fix does not perturb normal-path disposal
// counts (self-pass (b): still exactly once on the path that never throws).
BEZEL_TEST(generic_call_exception_unwind_array_double_free_2361,
           normal_exit_control_unaffected) {
    JitResult r = runAotChecked(program(/*doThrow=*/false));
    BEZEL_EXPECT_TRUE(r.ran);
    // 3 elements, no throw: F.Check counts all 3 and returns normally.
    BEZEL_EXPECT_EQ(r.exitCode, 3);
    for (int id = 0; id <= 2; ++id) {
        std::string marker = "D" + std::to_string(id);
        BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, marker), 1);
    }
}
