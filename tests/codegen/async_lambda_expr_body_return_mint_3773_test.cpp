// Codegen / memory-unsafety — issue #3773 (spec §27.3 "Lambda expressions",
// §27.5 "Closures", §28.9 "Runtime-managed types", §30.6 "Async lambdas",
// v0.286): an ASYNC lambda whose body is an EXPRESSION (`async lambda() =>
// <expr>`, not a `do … end` block) completes its outer task with a value it
// never minted a +1 for. When that value is a closure-typed BORROW — a
// captured outer closure local named by identifier, a closure-typed field
// read, or a choice between such reads — the awaited result is a +0 alias of
// a cell somebody else still owns, and the first consumer that assumes "a
// closure local's slot owns +1" (the lambda ENVIRONMENT DESTRUCTOR, which
// releases a captured closure-typed cell keyed purely on its function type)
// frees it out from under the alias.
//
// ⚠ THE ISSUE'S OWN ROOT-CAUSE GUESS AND ONE-LINE REMEDY WERE WRONG, and its
// corrective comment is the authority: `emitAsyncLambda`'s expression-body
// branch (LlvmBackend_Async.cpp, "Emit the body from state 0") ran
// `lowerExpr` → `emitDisposals` → `emitAsyncComplete` → `CreateRetVoid` with
// NO ownership adjustment of ANY kind — `returnedClosureLocalOwnsPlusOne` was
// never called anywhere in that file. The proposed
// `closureLocalCandidates_.clear()` was implemented, rebuilt and measured by
// the #3757/#3762 lane and the repro crashed identically, because there was
// no mint for a stale candidate set to suppress. The fix this file pins adds
// the missing mint (mirroring `lowerLambda`'s own expression-body mint,
// LlvmBackend_Exprs2.cpp) AND the clear — which only then becomes
// load-bearing, since the new mint is exactly what a stale candidate hit
// would suppress.
//
// Rows 1-8 are `runAotChecked` (generated-code ASan with LeakSanitizer on),
// 200 activations, and leak-clean on BOTH sides — so LSan cannot mask the
// double-free the first three rows exist to catch, and rows 4-8 are real
// over-mint guards rather than same-both-sides noise.
//
// ⚠ ROW 9 IS DIFFERENT and is the honest half of this fix. It is
// `runAotCheckedLenient` (ASan on, LSan OFF), because its program is this
// fix's ONE ACCEPTED REGRESSION: an awaited result that NOTHING releases goes
// from clean to 8000 B / 200 allocations, one 40-byte closure cell per
// activation. That is the mint with no consumer. It is not a defect of the
// mint — the SYNC twin of the same program leaks the identical 8000 B / 200 on
// BOTH compilers, so the async venue has simply joined the sync venue's
// accounting — and it is the fail-safe direction
// `returnedClosureLocalOwnsPlusOne`'s own contract names ("at worst a leak,
// never a UAF/double-free"). The consumer-side half (`var g := await f();`
// recognising an awaited `Task<F>` result as a +1 closure producer) lives in
// `collectClosureReleaseEligible` / `isUserMethodClosureReturnCall` and is
// tracked on issue 2714; when that lands, row 9 should move to
// `runAotChecked` and this note should go away. Until then the row exists so
// the regression is PINNED somewhere in the test tree — with its measured
// number — instead of living only in a commit message.
//
// Measured on the parent commit (r12-cycle-1 = 6d322f7, a scratch-copy
// swap-and-rebuild of LlvmBackend_Async.cpp), `bezelc --sanitize=address`,
// `ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:exitcode=99`:
//
//   captured outer closure local, result re-captured   double-free (2 reports)
//   closure-typed FIELD read, result re-captured       double-free (2 reports)
//   `if`-expression over two captured closure locals   double-free (2 reports)
//   the five over-mint guard rows                      clean, 0 leaked
//
// AFTER the fix: all eight rows clean (0 leaked, exit 0) and the printed
// totals are unchanged.
//
// ⚠ NOT COVERED, deliberately: the BLOCK-bodied twin (`async lambda() => do
// return inner; end`) reproduces the SAME heap-use-after-free and is NOT
// fixed here — its `return` is lowered by `lowerReturnStmt`, whose async arm
// completes the task and returns before ever reaching the string/closure mint
// arms further down that function. That is a different file and a different
// venue; filed separately with a measured repro.
// per §27.3, §27.5, §28.9, §30.6 (issue #3773)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;   // row 9 only — see the header

namespace {
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// Row 9 only: ASan still catches a use-after-free / double-free in the emitted
// loads, but LeakSanitizer is off — the program's KNOWN 8000 B / 200 leak is
// the accepted regression the header describes, not the property under test.
std::string lenientOut(const std::string& prog) {
    auto r = runAotCheckedLenient(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

const char* kHdr = "module M; import System; import System.Threading.Tasks; ";

const char* kMain =
    "  public static async method Main(): Task<int> "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 200 do "
    "      total := total + await C.Run(); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(\"total=\" + total.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";
}  // namespace

// ── Row 1, the tightest pin: an async lambda's EXPRESSION body returns a
// CAPTURED outer closure local `inner` by identifier; the awaited result `g`
// is then captured by a sibling lambda local `use`, whose environment
// destructor releases the captured closure cell at Run's scope end. Pre-fix
// `g` was a +0 alias of `inner`'s single reference, which `f`'s own env
// destructor also releases — `attempting double-free`, twice per run. The
// mint gives `g` its own reference, so the two releases balance the two
// references. inner()=7, total = 200*7 = 1400.
// per §27.5, §28.9, §30.6 (issue #3773)
BEZEL_TEST(async_lambda_expr_body_return_mint_3773,
           captured_closure_local_mints_plus_one_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var inner := lambda() => 7; "
        "    var f := async lambda() => inner; "
        "    var g := await f(); "
        "    var use := lambda() => g(); "
        "    return use(); "
        "  end Run; " +
        kMain),
        "total=1400\n");
}

// ── Row 2, a closure-typed FIELD read (`=> this.handler`) rather than a
// captured local — a borrow of the object's reference, which the object's
// own disposal releases. Same double-free pre-fix, and it proves the fix is
// not keyed to the identifier/capture shape alone: the mint fires for any
// closure-typed body the three transfer predicates do not exclude.
// per §27.5, §28.9, §30.6 (issue #3773)
BEZEL_TEST(async_lambda_expr_body_return_mint_3773,
           closure_typed_field_read_mints_plus_one_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  private handler: () -> int; "
        "  public constructor() "
        "    handler := lambda() => 7; "
        "  end constructor; "
        "  public async method Go(): Task<int> "
        "    var f := async lambda() => this.handler; "
        "    var g := await f(); "
        "    var use := lambda() => g(); "
        "    return use(); "
        "  end Go; "
        "  public static async method Run(): Task<int> "
        "    using var c := new C(); "
        "    return await c.Go(); "
        "  end Run; " +
        kMain),
        "total=1400\n");
}

// ── Row 3, a CONDITIONAL body (`=> (if b then a1 else a2)`) over two
// captured closure locals. Neither arm is a +1 producer and the whole
// expression is not an identifier, so `returnedClosureLocalOwnsPlusOne`
// cannot answer for it at all — the mint must key on the body's TYPE, not on
// a recognized identifier shape. Double-free pre-fix. a1()=7.
// per §27.5, §28.9, §30.6, §20 (issue #3773)
BEZEL_TEST(async_lambda_expr_body_return_mint_3773,
           conditional_over_captured_closures_mints_plus_one_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var a1 := lambda() => 7; "
        "    var a2 := lambda() => 9; "
        "    var f := async lambda() => (if true then a1 else a2); "
        "    var g := await f(); "
        "    var use := lambda() => g(); "
        "    return use(); "
        "  end Run; " +
        kMain),
        "total=1400\n");
}

// ── Row 4, OVER-MINT GUARD: a fresh LAMBDA LITERAL body already carries its
// own +1 (`isPlusOneClosureProducer`). A second mint here would leave the
// cell one reference above zero after `use`'s env destructor releases it —
// a 40-byte leak per activation (8000 B / 200) that LSan would report.
// Clean on BOTH sides; it fails only if the fix double-mints.
// per §27.5, §28.9 (issue #3773)
BEZEL_TEST(async_lambda_expr_body_return_mint_3773,
           lambda_literal_body_is_not_double_minted_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var f := async lambda() => lambda() => 7; "
        "    var g := await f(); "
        "    var use := lambda() => g(); "
        "    return use(); "
        "  end Run; " +
        kMain),
        "total=1400\n");
}

// ── Row 5, OVER-MINT GUARD: a METHOD REFERENCE used as a value also lowers
// to a fresh +1 cell (`isPlusOneClosureProducer`'s method-reference arm).
// per §27.5, §27.6, §28.9 (issue #3773)
BEZEL_TEST(async_lambda_expr_body_return_mint_3773,
           method_reference_body_is_not_double_minted_spec_27_6) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Seven(): int "
        "    return 7; "
        "  end Seven; "
        "  public static async method Run(): Task<int> "
        "    var f := async lambda() => C.Seven; "
        "    var g := await f(); "
        "    var use := lambda() => g(); "
        "    return use(); "
        "  end Run; " +
        kMain),
        "total=1400\n");
}

// ── Row 6, OVER-MINT GUARD: a USER-METHOD closure-returning CALL
// (`isUserMethodClosureReturnCall`) — the callee's ReturnStmt already minted.
// per §27.5, §28.9 (issue #3773)
BEZEL_TEST(async_lambda_expr_body_return_mint_3773,
           user_method_closure_call_body_is_not_double_minted_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Mk(): () -> int "
        "    return lambda() => 7; "
        "  end Mk; "
        "  public static async method Run(): Task<int> "
        "    var f := async lambda() => C.Mk(); "
        "    var g := await f(); "
        "    var use := lambda() => g(); "
        "    return use(); "
        "  end Run; " +
        kMain),
        "total=1400\n");
}

// ── Row 7, OVER-MINT GUARD, the AWAIT arm of this fix's own async-specific
// exclusion: `=> await C.MkA()` hands back the value the awaited task was
// completed with, which the completing body already owns. None of the sync
// venue's three transfer predicates recognizes an `await`, so without the
// arrives-from-a-callee exclusion this row leaked 8000 B / 200 (measured).
// per §27.5, §28.9, §30.1, §30.6 (issue #3773)
BEZEL_TEST(async_lambda_expr_body_return_mint_3773,
           awaited_task_result_body_is_not_double_minted_spec_30_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method MkA(): Task<() -> int> "
        "    return lambda() => 7; "
        "  end MkA; "
        "  public static async method Run(): Task<int> "
        "    var f := async lambda() => await C.MkA(); "
        "    var g := await f(); "
        "    var use := lambda() => g(); "
        "    return use(); "
        "  end Run; " +
        kMain),
        "total=1400\n");
}

// ── Row 8, OVER-MINT GUARD, the CALL arm of the same exclusion: `=> f2()`
// invokes a closure-typed LOCAL, whose own return already minted (the sync
// lambda venue's expression-body mint). `isUserMethodClosureReturnCall`
// answers only for a user MethodSymbol callee, so it does not cover this
// shape; without the exclusion this row leaked 8000 B / 200 (measured).
// per §27.5, §28.9 (issue #3773)
BEZEL_TEST(async_lambda_expr_body_return_mint_3773,
           closure_local_call_body_is_not_double_minted_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var f2 := lambda(): () -> int => lambda() => 7; "
        "    var f := async lambda() => f2(); "
        "    var g := await f(); "
        "    var use := lambda() => g(); "
        "    return use(); "
        "  end Run; " +
        kMain),
        "total=1400\n");
}

// ── Row 9, THE ACCEPTED REGRESSION, pinned rather than only described: the
// same captured-closure-local body as row 1, but the awaited result is simply
// CALLED and dropped — nothing captures it, so nothing ever releases the +1
// this fix now mints. Measured on the parent commit (r12-cycle-1 = 6d322f7):
// clean, 0 leaked. With the fix: 8000 B / 200 allocations, one 40-byte closure
// cell per activation. The SYNC twin of this program —
// `var f := lambda(): () -> int => inner; var g := f(); return g();` — leaks
// the identical 8000 B / 200 on BOTH compilers, which is why the mint is right
// and the leak is a consumer-side gap rather than an over-mint: issue 2714,
// "a closure local bound from a closure-value CALL owns nothing", whose async
// half (`var g := await f();`) is this row.
//
// `runAotCheckedLenient` (ASan on, LSan off) is deliberate and is the only row
// in this file that uses it: the assertion below is the VALUE, which proves
// the emitted code neither uses-after-free nor double-frees on this path — the
// property that actually changed. A row that merely leaks must not be allowed
// to hide a fault, and ASan still catches one here. When issue 2714 lands,
// this row moves to `runAotChecked` and the leak note is deleted.
// inner()=7, total = 200*7 = 1400. per §27.5, §28.9 (issue #3773, issue 2714)
BEZEL_TEST(async_lambda_expr_body_return_mint_3773,
           awaited_result_with_no_consumer_leaks_known_gap_2714_spec_28_9) {
    BEZEL_EXPECT_EQ(lenientOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var inner := lambda() => 7; "
        "    var f := async lambda() => inner; "
        "    var g := await f(); "
        "    return g(); "
        "  end Run; " +
        kMain),
        "total=1400\n");
}
