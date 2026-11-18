// Codegen / ownership + leak — issue #8804 (spec §28.3 "Local variables,
// ownership, and borrows", §28.3.10 "Owned-local discharge requirement",
// §28.9 "Runtime-managed types", §30.1 "`async` methods", §30.6 "Async
// lambdas", v0.359): an `async lambda` body's plain-`var` OWNED LOCAL was
// never disposed at its scope end, so the instance LEAKED on the normal
// completion path (16 B per call, LeakSanitizer "Direct leak of 16 byte(s)
// ... in bz$asynclam$0$Resume").
//
// THE ASYMMETRY, and where it is NOT. The issue's own hypothesis was that the
// async LAMBDA's resume lowering (src/codegen/LlvmBackend_Async.cpp's
// `emitAsyncLambda`) skips the owned local's dispose while the async METHOD's
// twin (`emitAsyncMethod`) does not. MEASURED: the two lowerings are
// equivalent here. `emitAsyncLambda` lowers the lambda body into the same
// resume-function `disposeStack_` scope `emitAsyncMethod` uses, and the
// scope-end disposal is registered by codegen's `registerUsingDisposal` —
// which runs for BOTH venues, driven by the ONE flag the ownership pass
// stamps on the declaration (`ast::LocalDecl::autoDropAtScopeEnd`). With that
// flag set, the async LAMBDA disposes its local correctly and leaks nothing;
// with it unset, the async METHOD's local leaks too.
//
// The flag is set by the ownership pass's affine auto-drop
// (`MethodOwnership::run`). That sweep has two shapes: the `endState` loop
// (the function exit) and the nested-scope fallback over
// `everOwnedPlainLocals_` — the latter for a plain owned local declared in a
// NESTED scope that never appears in `endState`. A LAMBDA BODY is exactly
// such a nested scope: the ownership pass analyses it in the
// `applyUsingVarBorrowPinInLambdas` pre-pass, which records the lambda's
// owned locals in the SHARED `everOwnedPlainLocals_` but not in the enclosing
// body's `states_` / `endState`. The fallback was gated
// `!isAsync_ && !sawYield_ && !isDestructor_`, so in an ASYNC body such a
// local was in NEITHER arm of the sweep: the async `endState` arm never saw
// it and the fallback was gated off. `autoDropAtScopeEnd` was therefore never
// stamped, codegen never registered the scope-end disposal, and the instance
// leaked.
//
// §28.3.10: "An owned local is disposed exactly once on every control-flow
// path … the compiler disposes the value at the end of its scope", and "The
// rule is uniform across body kinds"; §28.9: "the compiler inserts a free …
// at that holder's release point: scope end for locals and `using var`".
//
// Every row is `runAotChecked` (generated-code ASan with LeakSanitizer ON):
// a double free / use-after-free crashes the row before the counter is
// compared (ran=false -> "<did-not-run>", which fails the string match), and
// any leftover allocation (a leak) also fails the SAME way via LSan at exit
// (detect_leaks=1) — no separate leak assertion is needed.
//
// `Cell` bumps a static counter in its constructor and decrements it in its
// destructor, so `n` at exit is a direct oracle for "did the destructor run".
// 200 activations (row 1, the issue's own shape) / 100 activations (the
// variants), full drain.
//
// per §28.3, §28.3.10, §28.9, §30.1, §30.6 (issue #8804, v0.359)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// Cell: a disposable class with a static live-instance counter. ctor +1,
// destructor -1. Same shape as tests/codegen/async_lambda_scope_end_disposal_3728_test.cpp.
const char* kHdr =
    "module M; import System; import System.Threading; "
    "import System.Threading.Tasks; "
    "class Cell "
    "  public static n: int := 0; "
    "  public V: int := 0; "
    "  public constructor(v: int) Cell.n := Cell.n + 1; V := v; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";

// The tail of `Main`: sum the per-activation results and print the counter.
std::string mainTail(int iterations) {
    return std::string(
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<") + std::to_string(iterations) +
        " do total := total + await C.Run(); end; "
        "    Console.WriteLine(\"total=\" + total.ToString() + "
        "                      \" n=\" + Cell.n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
}
}  // namespace

// ── Row 1, the PRIMARY repro from the issue: an `async lambda` whose body
// owns a local class instance and completes normally. The issue's own program
// (`var u := new Cell(1); return u.value;`), driven 200 times so the leak is
// 200 x 16 B. Pre-fix: LSan reports "Direct leak of 16 byte(s) ... in
// bz$asynclam$0$Resume" and the row fails via ran=false ("<did-not-run>").
// per §28.3.10, §28.9, §30.6 (issue #8804)
BEZEL_TEST(async_lambda_owned_local_scope_end_8804,
           plain_var_owned_local_disposed_on_normal_edge_spec_28_3_10) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            std::string(kHdr) +
            "class C "
            "  public static async method Run(): Task<int> "
            "    var f := async lambda(): Task<int> => do var u := new Cell(1); return u.V; end; "
            "    return await f(); "
            "  end Run; " +
            mainTail(200)),
        "total=200 n=0\n");
}

// ── Row 2, the AWAIT-BEFORE variant: the local is declared AFTER a
// suspension point, so the lambda's frame is spilled and restored around the
// `await` before the local exists. The scope-end disposal must still fire on
// the activation that completes the task.
// per §28.3.10, §30.1, §30.6 (issue #8804)
BEZEL_TEST(async_lambda_owned_local_scope_end_8804,
           owned_local_declared_after_an_await_is_disposed_spec_30_1) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            std::string(kHdr) +
            "class C "
            "  public static async method Run(): Task<int> "
            "    var f := async lambda(): Task<int> => do await Task.Delay(1); var u := new Cell(1); return u.V; end; "
            "    return await f(); "
            "  end Run; " +
            mainTail(100)),
        "total=100 n=0\n");
}

// ── Row 3, the AWAIT-AFTER variant: the local is declared BEFORE a
// suspension point, so it is live across the `await` and the disposal runs
// on the activation that resumes and completes.
// per §28.3.10, §30.1, §30.6 (issue #8804)
BEZEL_TEST(async_lambda_owned_local_scope_end_8804,
           owned_local_live_across_an_await_is_disposed_spec_30_1) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            std::string(kHdr) +
            "class C "
            "  public static async method Run(): Task<int> "
            "    var f := async lambda(): Task<int> => do var u := new Cell(1); await Task.Delay(1); return u.V; end; "
            "    return await f(); "
            "  end Run; " +
            mainTail(100)),
        "total=100 n=0\n");
}

// ── Row 4, TWO locals in the same body: each is disposed exactly once
// (2 x 100 instances; `n` is the live count at exit, so it must be 0).
// per §28.3.10, §28.9 (issue #8804)
BEZEL_TEST(async_lambda_owned_local_scope_end_8804,
           two_owned_locals_are_each_disposed_exactly_once_spec_28_3_10) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            std::string(kHdr) +
            "class C "
            "  public static async method Run(): Task<int> "
            "    var f := async lambda(): Task<int> => do var u := new Cell(1); var w := new Cell(2); return u.V + w.V; end; "
            "    return await f(); "
            "  end Run; " +
            mainTail(100)),
        "total=300 n=0\n");
}

// ── Row 5, the BODY-KIND SIBLING: the SAME nested-scope defect reached a SYNCHRONOUS
// lambda nested in an ASYNC method. The base's `!isAsync_` conjunct skipped the
// nested-scope auto-drop for EVERY plain owned local in a nested scope of an ASYNC
// body — not only for an `async lambda`'s — so this shape leaked the same 16 B per
// call. The discriminator is the ENCLOSING body's kind, not the lambda's: row 6's
// sync-lambda-in-a-sync-method twin was already correct on the base.
// per §28.3.10, §28.9 (issue #8804)
BEZEL_TEST(async_lambda_owned_local_scope_end_8804,
           sync_lambda_nested_in_an_async_method_owns_its_local_spec_28_3_10) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            std::string(kHdr) +
            "class C "
            "  public static async method Run(): Task<int> "
            "    var f := lambda(): int => do var u := new Cell(1); return u.V; end; "
            "    return f(); "
            "  end Run; " +
            mainTail(100)),
        "total=100 n=0\n");
}

// ── Row 6, the OVER-RELEASE BOUNDARY CONTROL: the local is MOVED OUT of
// the body (`return u;` into an `OwningTask<Cell>`), so §28.3.10's
// "the move analysis … leaves a transferred value in moved-from state so the
// scope-end disposal skips it" applies — the scope-end disposal must NOT
// free it, and the awaiter's `using var` must free it exactly once.
//
// This row is GREEN on the base (the base registers no scope-end disposal at
// all, so it cannot double-free) and is kept as the suite's ONE boundary
// control because a corpus case cannot express an ASan double-free leg. It
// goes RED under the plausible wrong fix this issue invites: stamping
// `autoDropAtScopeEnd` WITHOUT honouring the move-out (i.e. a scope-end
// disposal that frees the slot unconditionally rather than null-guarded on
// the transfer) would free the cell the task still owns — a heap-use-after-
// free / double free, which ASan reports and the row fails.
// per §28.3.10, §28.8.1, §30.5, §30.6 (issue #8804)
BEZEL_TEST(async_lambda_owned_local_scope_end_8804,
           moved_out_local_is_not_freed_by_the_scope_end_disposal_spec_28_8_1) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            std::string(kHdr) +
            "class C "
            "  public static async method Run(): Task<int> "
            "    var f := async lambda(): OwningTask<Cell> => do var u := new Cell(1); return u; end; "
            "    using var c := await f(); "
            "    return c.V; "
            "  end Run; " +
            mainTail(100)),
        "total=100 n=0\n");
}
