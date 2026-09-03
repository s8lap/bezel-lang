// Codegen / memory-unsafety — issue #3788 (spec §27.3 "Lambda expressions",
// §27.5 "Closures", §28.9 "Runtime-managed types", §30.5 "Result lifetime and
// task disposal", v0.286): an ASYNC lambda whose body is a BLOCK (`async
// lambda() => do ... end`, not `=> <expr>`) completes its outer task with a
// value it never minted a +1 for. When that value is a closure-typed BORROW —
// a captured outer closure local named by identifier, a closure-typed field
// read, or a choice between such reads — the awaited result is a +0 alias of
// a cell somebody else still owns, and the first consumer that assumes "a
// closure local's slot owns +1" (the lambda ENVIRONMENT DESTRUCTOR) frees it
// out from under the alias.
//
// Twin of #3773 (expression-body fix, landed in LlvmBackend_Async.cpp ~L2218).
// The fix this file pins goes in `lowerReturnStmt`'s async arm
// (LlvmBackend_Stmts.cpp ~L2362, between `emitAsyncPatternAResultDecision` and
// `emitAsyncComplete`), keying on the RETURNED EXPRESSION's own sema type
// (`ctx_.exprType(*rs->value)`) — NOT `asyncResultSemaType_`, which carries the
// outer `Task<T>` wrapping for an unannotated return. Same three transfer
// exclusions as #3773 (isPlusOneClosureProducer, isUserMethodClosureReturnCall,
// returnedClosureLocalOwnsPlusOne) plus the async-specific "arrives from a
// callee that mints on return" exclusion (CallExpr of any shape, or `await`).
//
// All rows are `runAotChecked` (generated-code ASan + LSan), 200 activations,
// and leak-clean on the FIX; LSan cannot mask the double-free rows 1-4 exist
// to catch. Row 9 is `runAotCheckedLenient` (ASan on, LSan off) for the same
// known gap #3773 carries: an awaited result that NOTHING releases goes from
// clean to 8000 B / 200 (40-byte closure cell per activation), matching the
// sync venue's identical measured leak — the consumer-side `var g := await
// f();` half is filed separately (issue 2714).
//
// Measured on the parent commit (r12-cycle-1 = 254ae52, a scratch-copy
// swap-and-rebuild of LlvmBackend_Stmts.cpp), `bezelc --sanitize=address`,
// `ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:exitcode=99`:
//
//   captured outer closure local, result re-captured   double-free (envdtor)
//   closure-typed FIELD read, same consumer           double-free (envdtor)
//   `if`-expression over two captured closure locals   double-free (envdtor)
//   async method `return this.handler;` (the related case) double-free
//   the five over-mint guard rows                      clean, 0 leaked
//
// AFTER the fix: all rows clean (0 leaked, exit 0) and the printed totals
// are unchanged. The envdtor no longer releases the cell the awaited result
// still borrows, because the result carries its own +1 from the new mint.
// per §27.3, §27.5, §28.9, §30.5 (issue #3788)

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
// the accepted regression (consumer-side `var g := await f();` gap, 2714).
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

// ── Row 1, the tightest pin: a BLOCK-bodied async lambda (`do ... end`)
// returns a CAPTURED outer closure local `inner` by identifier; the awaited
// result `g` is then captured by a sibling lambda local `use`, whose envdtor
// releases the captured closure cell at Run's scope end. Pre-fix `g` was a
// +0 alias of `inner`'s single reference, which the async lambda's own env
// destructor ALSO releases — `attempting double-free ... in bz$lam$1$envdtor`.
// The mint gives `g` its own reference, so the two releases balance the two
// references. inner()=7, total = 200*7 = 1400.
// per §27.5, §28.9, §30.5 (issue #3788)
BEZEL_TEST(async_lambda_block_body_return_mint_3788,
           captured_closure_local_mints_plus_one_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var inner := lambda() => 7; "
        "    var f := async lambda() => do return inner; end; "
        "    var g := await f(); "
        "    var use := lambda() => g(); "
        "    return use(); "
        "  end Run; " +
        kMain),
        "total=1400\n");
}

// ── Row 2, a closure-typed FIELD read (`=> do return this.handler; end`)
// rather than a captured local — a borrow of the object's reference, which
// the object's own disposal releases. Same double-free pre-fix; proves the
// fix is not keyed to the identifier/capture shape alone.
// per §27.5, §28.9, §30.5 (issue #3788)
BEZEL_TEST(async_lambda_block_body_return_mint_3788,
           closure_typed_field_read_mints_plus_one_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  private handler: () -> int; "
        "  public constructor() "
        "    handler := lambda() => 7; "
        "  end constructor; "
        "  public async method Go(): Task<int> "
        "    var f := async lambda() => do return this.handler; end; "
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

// ── Row 3, a CONDITIONAL body (`do return (if b then a1 else a2); end`) over
// two captured closure locals. Neither arm is a +1 producer and the whole
// expression is not an identifier, so `returnedClosureLocalOwnsPlusOne`
// cannot answer for it — the mint must key on the body's TYPE, not on a
// recognized identifier shape. Double-free pre-fix. a1()=7.
// per §27.5, §28.9, §30.5, §20 (issue #3788)
BEZEL_TEST(async_lambda_block_body_return_mint_3788,
           conditional_over_captured_closures_mints_plus_one_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var a1 := lambda() => 7; "
        "    var a2 := lambda() => 9; "
        "    var f := async lambda() => do return (if true then a1 else a2); end; "
        "    var g := await f(); "
        "    var use := lambda() => g(); "
        "    return use(); "
        "  end Run; " +
        kMain),
        "total=1400\n");
}

// ── Row 4, the RELATED CASE: an async METHOD returning a closure-typed field
// (`async method Go(): Task<() -> int> return this.handler;`). The same
// `lowerReturnStmt`'s async arm handles it (the path is shared between
// block-bodied lambdas and async methods), and it was a +0 borrow pre-fix
// because the arm completed the task without minting. The consumer here is
// `use`'s envdtor; the double-free is in the method's own disposal (the
// object releasing its own `handler` field). Proves the fix covers both
// lambdas and methods via the same arm.
// per §27.5, §28.9, §30.5 (issue #3788)
BEZEL_TEST(async_lambda_block_body_return_mint_3788,
           async_method_closure_return_mints_plus_one_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  private handler: () -> int; "
        "  public constructor() "
        "    handler := lambda() => 7; "
        "  end constructor; "
        "  public async method Go(): Task<() -> int> "
        "    return this.handler; "
        "  end Go; "
        "  public static async method Run(): Task<int> "
        "    using var c := new C(); "
        "    var g := await c.Go(); "
        "    var use := lambda() => g(); "
        "    return use(); "
        "  end Run; " +
        kMain),
        "total=1400\n");
}

// ── Row 5, OVER-MINT GUARD: a fresh LAMBDA LITERAL body already carries its
// own +1 (`isPlusOneClosureProducer`). A second mint here would leave the
// cell one reference above zero after `use`'s env destructor releases it —
// a 40-byte leak per activation (8000 B / 200) that LSan would report.
// Clean on BOTH sides; fails only if the fix double-mints.
// per §27.5, §28.9 (issue #3788)
BEZEL_TEST(async_lambda_block_body_return_mint_3788,
           lambda_literal_body_is_not_double_minted_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var f := async lambda() => do return lambda() => 7; end; "
        "    var g := await f(); "
        "    var use := lambda() => g(); "
        "    return use(); "
        "  end Run; " +
        kMain),
        "total=1400\n");
}

// ── Row 6, OVER-MINT GUARD: a METHOD REFERENCE used as a value also lowers
// to a fresh +1 cell (`isPlusOneClosureProducer`'s method-reference arm).
// per §27.5, §27.6, §28.9 (issue #3788)
BEZEL_TEST(async_lambda_block_body_return_mint_3788,
           method_reference_body_is_not_double_minted_spec_27_6) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Seven(): int "
        "    return 7; "
        "  end Seven; "
        "  public static async method Run(): Task<int> "
        "    var f := async lambda() => do return C.Seven; end; "
        "    var g := await f(); "
        "    var use := lambda() => g(); "
        "    return use(); "
        "  end Run; " +
        kMain),
        "total=1400\n");
}

// ── Row 7, OVER-MINT GUARD: a USER-METHOD closure-returning CALL
// (`isUserMethodClosureReturnCall`) — the callee's ReturnStmt already minted.
// per §27.5, §28.9 (issue #3788)
BEZEL_TEST(async_lambda_block_body_return_mint_3788,
           user_method_closure_call_body_is_not_double_minted_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Mk(): () -> int "
        "    return lambda() => 7; "
        "  end Mk; "
        "  public static async method Run(): Task<int> "
        "    var f := async lambda() => do return C.Mk(); end; "
        "    var g := await f(); "
        "    var use := lambda() => g(); "
        "    return use(); "
        "  end Run; " +
        kMain),
        "total=1400\n");
}

// ── Row 8, OVER-MINT GUARD: a closure-LOCAL call (`=> do return f2(); end`)
// already minted in the inner lambda's expression-body mint (or here, if
// f2 is a sync lambda expression body, in lowerLambda's mint at L2485). The
// `arrivesFromCallee` exclusion recognizes this shape. Without it: 8000 B /
// 200 (measured). The sync twin of this row was the #3773-added test row 8.
// per §27.5, §28.9, §30.5 (issue #3788)
BEZEL_TEST(async_lambda_block_body_return_mint_3788,
           closure_local_call_body_is_not_double_minted_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var f2 := lambda(): () -> int => lambda() => 7; "
        "    var f := async lambda() => do return f2(); end; "
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
// this fix now mints. Measured on the parent commit (r12-cycle-1 = 254ae52):
// clean, 0 leaked. With the fix: 8000 B / 200 allocations, one 40-byte closure
// cell per activation. The SYNC twin of this program leaks the identical 8000
// B / 200 on BOTH compilers (this is the consumer-side gap, 2714).
//
// `runAotCheckedLenient` (ASan on, LSan off) is deliberate: the assertion
// below is the VALUE, which proves the emitted code neither uses-after-free
// nor double-frees on this path — the property that actually changed. A row
// that merely leaks must not be allowed to hide a fault, and ASan still
// catches one here. When 2714 lands, this row moves to `runAotChecked`.
// inner()=7, total = 200*7 = 1400.
// per §27.5, §28.9, §30.5 (issue #3788, issue 2714)
BEZEL_TEST(async_lambda_block_body_return_mint_3788,
           awaited_result_with_no_consumer_leaks_known_gap_2714_spec_28_9) {
    BEZEL_EXPECT_EQ(lenientOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Run(): Task<int> "
        "    var inner := lambda() => 7; "
        "    var f := async lambda() => do return inner; end; "
        "    var g := await f(); "
        "    return g(); "
        "  end Run; " +
        kMain),
        "total=1400\n");
}
