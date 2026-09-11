// Codegen — fix #4285: a Pattern-A `Task<T:class>` async producer whose
// non-`await` `return` is a BORROW (a field, property, or indexer read) was
// treated as §30.5 kind 1 ("a fresh owned value") by default, so the outer
// task's teardown disposed a value it never owned — a measured
// heap-use-after-free once the value's real owner disposed it independently.
//
// Spec (revision v0.298):
//   §30.5 "Result lifetime and task disposal", *Producer-side return-type
//   rules*, **Pattern A result slots are of three kinds**:
//     kind 1 — "A fresh owned value (a `return` of an owned expression): the
//       task owns the result and its destructor disposes it directly."
//     kind 2 — "`return await` over an owned Pattern A source — the chained
//       slot" (a `return await <source>` form only).
//     kind 3 — "`return await` over a borrowed source" (also a
//       `return await <source>` form only).
//   No fourth kind exists, and neither kind 2 nor kind 3 admits a BARE
//   (non-`await`) return at all — a non-`await` return must therefore satisfy
//   kind 1's parenthetical: the operand must be an OWNED expression.
//
//   §28.3 "Local variables, ownership, and borrows", *Producers* table:
//     "Field access `obj.f` where `f` is not `owned`: Borrowed (source:
//      `obj`)"; "Field access `obj.f` where `f` is `owned`: Borrowed (source:
//      `obj`)" (only "`move obj.f` where `f` is `owned`" is Owned); "Indexer
//      access `coll[i]`: Borrowed …".
//
// THE DEFECT. `detectAsyncPatternAKind` (LlvmBackend_Async.cpp) recognized
// kind 2 / kind 3 only for a top-level `await <operand>` return value; for
// EVERY other shape it set neither `asyncChainedReturn_` nor
// `asyncBorrowedReturn_`, so `emitAsyncPatternAResultDecision`'s
// `else if (asyncPatternAResultDisposer_)` arm (LlvmBackend_Stmts.cpp) always
// attached the kind-1 per-T result disposer — with NOTHING having verified
// the returned expression was actually OWNED. A field/property/indexer read
// is unconditionally a borrow per the Producers table above (regardless of
// whether the underlying field is itself `owned` — only `move` transfers
// it), so a producer shaped like
//
//     public static async method GetAsync(h: Holder): Task<Cell>
//       return h.item;   // h.item is a BORROW, not owned
//     end GetAsync;
//
// mints a task that, at its own teardown, disposes `h.item`'s referent a
// SECOND time (once through the disposer wrongly attached here, once through
// whatever the true owner does with it) — a use-after-free once the real
// owner's dispose runs first.
//
// MEASURED, this parent (§29 verification — reproduces verbatim on the
// current tree, 10 commits after #4285 was filed):
//   `scripts/agent_check.sh --probe --asan` on the issue's exact repro (300
//   iterations) — `AddressSanitizer: heap-use-after-free … READ of size 8 …
//   #0 bz$elemDispose … #1 bz_task_free
//   src/runtime/bezel_runtime_concurrency.cpp:1156 … #2
//   bz$async$M.C$OneIteration$Resume` — freed earlier by the explicit
//   `.Dispose()` the value's real owner performs.
//
// THE FIX (LlvmBackend_Async.cpp `detectAsyncPatternAKind`, free of the
// held ownership-pass files this run): when the return value is NOT itself
// `await <source>` (so kind 2 / kind 3 do not apply) and this body's Pattern
// A result disposer is active (`asyncPatternAResultDisposer_` — a no-op
// without it, so nothing to mis-attach), reject at compile time when the
// TOP-LEVEL return expression is one of the shapes the Producers table
// classifies as an UNCONDITIONAL borrow: `MemberAccessExpr`,
// `NullCondMemberExpr`, `IndexExpr`. Deliberately NOT extended to
// `IdentifierExpr` — a bare identifier may legitimately name a still-owned
// local or `owned` parameter (§28.3 *Factory methods and sourceless
// returns*'s admitted-source list includes "an owned frame local or `owned`
// parameter"), and proving that needs the ownership pass's own per-local
// flow state, which lives only in the two files this run holds for other
// lanes (`src/ownership/Ownership.cpp` #4286, `src/ownership/
// Ownership_Checks.cpp` #4247) — narrowing to the AST shapes the Producers
// table marks unconditional avoids both a duplicated/drifting classification
// (LANE_RULES §6) and a false-reject on that axis (proven accept-side below).
//
// FAIL-WITHOUT / PASS-WITH (measured via a commit-fix, revert-src, rebuild,
// re-run, restore-src, rebuild cycle): 3 of 3 rejection rows below
// (`reject_borrowed_field_return_30_5`,
// `reject_borrowed_indexer_return_30_5`,
// `reject_unconditional_even_when_never_awaited_30_5`) compile CLEAN
// (ran == true) without the fix and are rejected with a real location and
// this fix's diagnostic text WITH it. The four accept-side control rows
// pass identically on both compilers (deliberately unaffected axes — see
// each row's own comment for which).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Cell: a class with a static counter (ctor ++, dtor --) for exact,
// deterministic dispose-exactly-once verification — the same idiom
// tests/codegen/async_task_result_disposal_1503_test.cpp (#1503) uses for
// this exact family of Pattern-A disposal question.
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";

// A non-owned class-typed field: `Box.item` is a plain (borrow) field per
// §28.3 Producers ("Field access `obj.f` where `f` is not `owned`:
// Borrowed") — the exact shape #4285's `Holder.item` repro uses.
const char* kBox =
    "class Box public item: Cell; "
    "  public constructor(c: Cell) this.item := c; end constructor; "
    "end Box; ";

// The distinctive diagnostic-text fragment this fix's rejection carries —
// distinguishing it from any OTHER, pre-existing reason a program might fail
// to compile (see accept_pattern_b_borrow_return_rejected_by_existing_rule).
constexpr const char* kFixDiagFragment =
    "§30.5 Pattern A) may only `return` an OWNED expression";
}  // namespace

// ---------------------------------------------------------------------------
// Main repro row (#4285, minimized to a single call — the compile-time
// rejection does not need 300 iterations; the issue's own loop was to force
// the PRE-fix runtime UAF to manifest reliably under ASan's allocator reuse).
// ---------------------------------------------------------------------------
BEZEL_TEST(async_pattern_a_producer_borrow_return_4285,
          reject_borrowed_field_return_30_5) {
    auto r = runAot(
        "module M; " + std::string(kCell) + std::string(kBox) +
        "class C "
        "  public static async method GetAsync(h: Box): Task<Cell> "
        "    return h.item; "  // BORROW (h.item), not owned — no admitted
                                // producer-side shape for a bare (non-`await`)
                                // return of it (§30.5 kind 1/2/3)
        "  end GetAsync; "
        "  public static async method Main(): Task<int> "
        "    var x := new Cell(); "
        "    var h := new Box(x); "
        "    using var t := C.GetAsync(h); "
        "    var y := await t; "
        "    y.Dispose(); "
        "    x.Dispose(); "
        "    h.Dispose(); "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

// Sibling AST shape — an indexer read (`coll[i]`) is the same unconditional
// borrow per §28.3 Producers ("Indexer access `coll[i]`: Borrowed …").
BEZEL_TEST(async_pattern_a_producer_borrow_return_4285,
          reject_borrowed_indexer_return_30_5) {
    auto r = runAot(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method GetAsync(arr: Array<Cell>): Task<Cell> "
        "    return arr[0]; "
        "  end GetAsync; "
        "  public static async method Main(): Task<int> "
        "    using var arr := new Array<Cell>(); "
        "    var c := new Cell(); "
        "    using var t := C.GetAsync(arr); "
        "    var y := await t; "
        "    c.Dispose(); "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

// The rejection is PRODUCER-SIDE and unconditional: it fires from
// `GetAsync`'s own body regardless of whether any caller ever awaits (or
// even calls) it — proven by declaring `t` and never awaiting it.
BEZEL_TEST(async_pattern_a_producer_borrow_return_4285,
          reject_unconditional_even_when_never_awaited_30_5) {
    auto r = runAot(
        "module M; " + std::string(kCell) + std::string(kBox) +
        "class C "
        "  public static async method GetAsync(h: Box): Task<Cell> "
        "    return h.item; "
        "  end GetAsync; "
        "  public static async method Main(): Task<int> "
        "    var x := new Cell(); "
        "    var h := new Box(x); "
        "    using var t := C.GetAsync(h); "  // never awaited
        "    x.Dispose(); "
        "    h.Dispose(); "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

// ---------------------------------------------------------------------------
// Accept-side controls (§15 / LANE_RULES: the reject-side FAIL-WITHOUT proof
// says nothing about programs that must keep compiling).
// ---------------------------------------------------------------------------

// Kind 1's genuine case — a FRESH owned value (`new Cell()`, not a borrow)
// — is completely unaffected: it must still compile, run, and dispose the
// produced Cell exactly once at the task's own teardown (the #1503
// behaviour this fix does not touch). Loop-amplified (50x) so a leak or a
// double-free/UAF corrupting the counter cannot pass by accident.
BEZEL_TEST(async_pattern_a_producer_borrow_return_4285,
          accept_fresh_owned_new_expr_return_unaffected_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method MakeAsync(): Task<Cell> "
        "    return new Cell(); "  // kind 1: genuinely owned
        "  end MakeAsync; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 50 do "
        "      do using var t := C.MakeAsync(); var c := await t; end; "
        "      i := i + 1; "
        "    end; "
        "    return Cell.n; "  // expect 0: every iteration's Cell disposed
                               // exactly once
        "  end Main; "
        "end C;"),
        0);
}

// A bare IdentifierExpr return of a still-owned local (`return c;`, not
// `new Cell()` directly) is deliberately NOT touched by this fix (see the
// file header) — it must keep compiling and disposing correctly, proving no
// false-reject regression on the one axis this fix stays conservative about.
BEZEL_TEST(async_pattern_a_producer_borrow_return_4285,
          accept_bare_owned_local_return_unaffected_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method MakeAsync(): Task<Cell> "
        "    var c := new Cell(); "
        "    return c; "  // IdentifierExpr — not one of the flagged shapes
        "  end MakeAsync; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 50 do "
        "      do using var t := C.MakeAsync(); var c := await t; end; "
        "      i := i + 1; "
        "    end; "
        "    return Cell.n; "  // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// Pattern B (`OwningTask<T>`) is structurally out of this fix's gate
// (`patternAResultDisposerFor` excludes `OwningTask` by name — Pattern B
// carries no per-T Pattern-A disposer to mis-attach), so the identical
// borrowed-field-return shape must still be rejected by whichever rule
// already covered it, UNCHANGED — proven by asserting the OTHER rule's own
// diagnostic text fires, and this fix's text does NOT.
BEZEL_TEST(async_pattern_a_producer_borrow_return_4285,
          accept_pattern_b_borrow_return_rejected_by_existing_rule_30_5) {
    auto r = runAot(
        "module M; " + std::string(kCell) + std::string(kBox) +
        "class C "
        "  public static async method GetAsyncB(h: Box): OwningTask<Cell> "
        "    return h.item; "
        "  end GetAsyncB; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "may not be returned through the owning-type return"));
    BEZEL_EXPECT_TRUE(!diagnosticMessageContains(r, kFixDiagFragment));
}

// A legitimate kind-2 chained return (`return await <owned Pattern A
// source>`) is untouched: `detectAsyncPatternAKind` returns before reaching
// this fix's new branch whenever the top-level return value IS an `await`.
BEZEL_TEST(async_pattern_a_producer_borrow_return_4285,
          accept_chained_await_return_unaffected_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method LoadAsync(): Task<Cell> "
        "    return new Cell(); "
        "  end LoadAsync; "
        "  public static async method ChainAsync(): Task<Cell> "
        "    using var inner := C.LoadAsync(); "
        "    return await inner; "  // kind 2: chained slot
        "  end ChainAsync; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 50 do "
        "      do using var t := C.ChainAsync(); var c := await t; end; "
        "      i := i + 1; "
        "    end; "
        "    return Cell.n; "  // expect 0
        "  end Main; "
        "end C;"),
        0);
}
