// Codegen — issue #2731 item 1: `asyncChainedReturn_` was not covered by
// `Emitter::NestedBodyScope` (#2702), so a lazily-emitted, MEMOIZED nested body
// lowered in the middle of a `return await <call>` CONSUMED the caller's
// chained-return flag. The result is a DOUBLE DISPOSE of the awaited Pattern A
// result — a heap-use-after-free under ASan and a SIGSEGV on the plain oracle.
//
// ⚠ The issue filed this as a LEAK of the source `Task`, on the reading that
// `lowerAwait` suppresses the source-task free and only the *return* loses the
// flag. That reading is WRONG about the order and therefore about the severity,
// which is INVERTED (a double-free is worse than a leak). Corrected below.
//
// Spec (revision v0.240):
//   §23.3 "Block-form properties" — "Each accessor uses `=> expression` or
//     `=> do ... end`." A `do ... end` accessor is a user statement body
//     lowered into its OWN memoized function, so its `return` runs the whole
//     ReturnStmt lowering path — including that path's flag bookkeeping.
//   §23.4 "Indexers" — the same accessor forms for `property [i: int]: T`.
//   §30.5 "Result lifetime and task disposal", Pattern A kind 2 (the CHAINED
//     SLOT) — "`return await` over an owned Pattern A source … ownership of the
//     **source task** transfers into the outer task's state (a **chained**
//     slot), and the outer destructor disposes the chained source task, never
//     the borrowed result directly. … On both paths the source task is disposed
//     exactly once — by the outer task's destructor, transitively."
//
// THE DEFECT, precisely. `LlvmBackend_Stmts.cpp` ~1826 sets `asyncChainedReturn_`
// when it recognises `return await <call>` in an async body with a pointer-typed
// result. The flag is read back TWICE, and — this is the part the issue got
// backwards — BOTH reads happen AFTER the return value's operand is lowered:
//   * `lowerAwait` (LlvmBackend_Async.cpp ~2932) lowers its operand FIRST
//     (~2912) and only then computes
//     `freeTaskAfter = isCall && !asyncChainedReturn_`;
//   * the ReturnStmt itself (Stmts.cpp ~1924) reads-and-clears UNCONDITIONALLY,
//     with no `smKind_`/`smStateStruct_` gate.
// A block-bodied getter/indexer read inside the awaited call's ARGUMENT is
// emitted during that operand lowering. Its own `return` reaches Stmts.cpp
// ~1924 and clears the caller's flag. Both legitimate consumers then read
// `false`, so the emitted code:
//   * frees the SOURCE task at the await site (`bz_task_free`), which runs the
//     source task's `result_disposer` on the awaited result; and
//   * skips `bz_task_set_chained` and instead attaches a result disposer for
//     the SAME object to the OUTER task,
// which disposes the result a second time when the outer task is freed.
//
// OBSERVED pre-fix, on the repro below (bezelc, plain and --sanitize=address):
//     ERROR: AddressSanitizer: heap-use-after-free … READ of size 8
//       #0 bz$elemDispose
//       #1 bz_task_free
//       #2 bz_task_fire_and_forget
//       #3 bz$async$Main$Resume
//     freed by thread T0 here: bz$elemDispose <- bz$async$Outer$Resume
//   and SIGSEGV (exit 139) on the plain, non-instrumented run.
// IR census pre-fix vs post-fix for kBlockGetterInAwaitOperand:
//     bz_task_set_chained  0 -> 1      bz_task_free              2 -> 0
//     bz_task_set_result_disposer  2 -> 1
// i.e. exactly the Pattern A kind 2 chained slot the spec requires, and the
// same census the two controls below already produced pre-fix.
//
// THE FIX. `asyncChainedReturn_` joins `NestedBodyScope`'s "narrow per-call
// channels" — the same transient publish/consume class as `lastCallGenericSubst_`,
// which the guard already covered. Saved, cleared at entry, restored on exit, so
// a nested body's `return` consumes its own flag and never the caller's.
//
// WHY THE COUNTER ORACLE AND NOT LSan. Per the issue's own caution, an LSan
// verdict on this shape can be binary-layout dependent, so absence of a report
// is not proof. Every case below instead proves reclaim POSITIVELY through a
// live-object counter folded into the exit code: `Cell.n * 100 + Cell.made`
// pins BOTH halves — how many `Cell`s were constructed and how many are still
// alive at the end — so a leak (alive > 0) and a double dispose (crash) are
// each observable as a wrong value rather than as a silent pass.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("SKIP") != std::string::npos) return true;
    }
    return false;
}

// Plain (non-ASan) AOT run: the exit code alone, or -999 when the compile or
// the run did not complete (a SIGSEGV is WIFSIGNALED -> ran=false). `--full`
// has masked live defects the plain gate catches (#2633 / #2629), so every case
// is checked on the plain oracle as well as the instrumented one.
int runPlain(const std::string& src) {
    JitResult r = bezel::test::runAot(src);
    return r.ran ? r.exitCode : -999;
}

// ASan+LSan run. A heap-use-after-free / double-free / leak surfaces as
// ran=false with the sanitizer report in `diagnostics`, i.e. -999.
int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return -1000;   // ASan toolchain absent — case skipped
    return r.ran ? r.exitCode : -999;
}

// A disposal-counted class. `n` is the live count, `made` the construction
// count; folding both into the exit code makes "disposed exactly once"
// observable as a VALUE, independently of any sanitizer verdict.
const char* kCell =
    "class Cell "
    "  public static n: int := 0; "
    "  public static made: int := 0; "
    "  public v: int; "
    "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; "
    "    Cell.made := Cell.made + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";

// A BLOCK-bodied (`=> do … end`) property — §23.3. Lowered by
// `ensurePropertyBlockGetter` into its own memoized function.
const char* kOwnerBlockProperty =
    "class Owner "
    "  private _n: int; "
    "  public constructor() _n := 7; end constructor; "
    "  public property N: int "
    "    get => do "
    "      return _n; "
    "    end; "
    "  end N; "
    "end Owner; ";

// The same accessor as a BLOCK-bodied INDEXER — §23.4. Lowered by
// `ensureIndexerBlockGetter`, the guard's second adopter (#2701).
const char* kOwnerBlockIndexer =
    "class Owner "
    "  private _n: int; "
    "  public constructor() _n := 7; end constructor; "
    "  public property [i: int]: int "
    "    get => do "
    "      return _n + i; "
    "    end; "
    "  end; "
    "end Owner; ";

// `Outer` is the §30.5 Pattern A producer whose `return await C.Inner(<arg>)`
// is the chained slot. `<arg>` is spliced in: the defect fires only when the
// argument's lowering emits a nested body for the FIRST time, which is why the
// controls below either pre-touch the accessor or use a form that is inlined.
std::string program(const std::string& ownerDecl, const std::string& outerBody) {
    return std::string("module M; ") + kCell + ownerDecl +
        "class C "
        "  public static async method Inner(k: int): Task<Cell> "
        "    return new Cell(k); "
        "  end Inner; "
        "  public static async method Outer(): Task<Cell> " + outerBody +
        "  end Outer; "
        "  public static async method Main(): Task<int> "
        "    do "
        "      using var t := C.Outer(); "
        "      var r := await t; "          // §30.5 Pattern A: a BORROW
        "    end; "
        "    return Cell.n * 100 + Cell.made; "   // 0 alive, 1 made => 1
        "  end Main; "
        "end C;";
}

}  // namespace

// ── (1) THE REPRO: a block-bodied PROPERTY read inside the awaited call's
// argument, so `ensurePropertyBlockGetter` runs mid-`return await`. Pre-fix:
// SIGSEGV on the plain oracle and heap-use-after-free under ASan (-999 both
// ways). Post-fix: 1 — one Cell constructed, zero alive at the end, i.e. the
// chained source task disposed the result exactly once (§30.5 Pattern A kind 2).
static const std::string kBlockGetterInAwaitOperand = program(
    kOwnerBlockProperty,
    "    using var o := new Owner(); "
    "    return await C.Inner(o.N); ");

BEZEL_TEST(codegen_async_chained_return_2731, block_getter_in_await_operand) {
    BEZEL_EXPECT_EQ(runPlain(kBlockGetterInAwaitOperand), 1);
}

BEZEL_TEST(codegen_async_chained_return_2731,
           block_getter_in_await_operand_checked) {
    int rc = runChecked(kBlockGetterInAwaitOperand);
    if (rc == -1000) return;   // ASan toolchain absent
    BEZEL_EXPECT_EQ(rc, 1);
}

// ── (2) The INDEXER spelling (§23.4), through the guard's second adopter
// `ensureIndexerBlockGetter`. Reproduced identically pre-fix (SIGSEGV / UAF),
// which is what establishes that the hole is in the SHARED guard rather than in
// one emitter.
static const std::string kBlockIndexerInAwaitOperand = program(
    kOwnerBlockIndexer,
    "    using var o := new Owner(); "
    "    return await C.Inner(o[0]); ");

BEZEL_TEST(codegen_async_chained_return_2731, block_indexer_in_await_operand) {
    BEZEL_EXPECT_EQ(runPlain(kBlockIndexerInAwaitOperand), 1);
}

BEZEL_TEST(codegen_async_chained_return_2731,
           block_indexer_in_await_operand_checked) {
    int rc = runChecked(kBlockIndexerInAwaitOperand);
    if (rc == -1000) return;   // ASan toolchain absent
    BEZEL_EXPECT_EQ(rc, 1);
}

// ── (3) AMPLIFIED POSITIVE RECLAIM PROOF. 64 iterations of the repro. A
// single-shot clean run proves little (the issue's own caution: an LSan verdict
// here can be layout dependent, and a UAF read of a just-freed block need not
// fault). 64 rounds of allocate/await/dispose put real allocator churn between
// each free and the next read, and the exit code pins BOTH counters: 0 alive
// and exactly 64 constructed => 0 * 100 + 64. A leak would report 100+ (and
// exceed nothing — 64 is well inside runAot's 8-bit exitCode range, per the
// harness's WEXITSTATUS masking).
static const std::string kAmplified = std::string("module M; ") + kCell +
    kOwnerBlockProperty +
    "class C "
    "  public static async method Inner(k: int): Task<Cell> "
    "    return new Cell(k); "
    "  end Inner; "
    "  public static async method Outer(): Task<Cell> "
    "    using var o := new Owner(); "
    "    return await C.Inner(o.N); "
    "  end Outer; "
    "  public static async method Main(): Task<int> "
    "    for var i in 0..63 do "
    "      using var t := C.Outer(); "
    "      var r := await t; "
    "    end; "
    "    return Cell.n * 100 + Cell.made; "   // 0 alive, 64 made => 64
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_async_chained_return_2731, amplified_64_rounds) {
    BEZEL_EXPECT_EQ(runPlain(kAmplified), 64);
}

BEZEL_TEST(codegen_async_chained_return_2731, amplified_64_rounds_checked) {
    int rc = runChecked(kAmplified);
    if (rc == -1000) return;   // ASan toolchain absent
    BEZEL_EXPECT_EQ(rc, 64);
}

// ── (4) OVER-CORRECTION CONTROL A: the SAME program with the accessor read one
// statement EARLIER, so the getter is already memoized by the time the
// `return await` runs and no nested emission happens inside the flag's live
// window. This passes BOTH pre-fix and post-fix; it is what proves the defect
// is the nested emission and not `Task<Cell>` chaining in general, and it
// guards the fix against neutering the chained slot itself.
static const std::string kPreTouchControl = program(
    kOwnerBlockProperty,
    "    using var o := new Owner(); "
    "    var pre := o.N; "
    "    return await C.Inner(pre); ");

BEZEL_TEST(codegen_async_chained_return_2731, pre_touched_getter_control) {
    BEZEL_EXPECT_EQ(runPlain(kPreTouchControl), 1);
}

// ── (5) OVER-CORRECTION CONTROL B: an EXPRESSION-bodied accessor (`get => _n;`,
// §23.3's other form) in the same position. An expression getter is INLINED at
// the read site rather than lowered into a separate function, so it never runs
// a nested ReturnStmt and never reached the flag — verified pre-fix, where this
// case already produced the chained-slot IR census. Passes both pre- and
// post-fix; recorded so the boundary of the defect is pinned in the suite.
static const std::string kExprGetterControl = program(
    "class Owner "
    "  private _n: int; "
    "  public constructor() _n := 7; end constructor; "
    "  public property N: int "
    "    get => _n; "
    "  end N; "
    "end Owner; ",
    "    using var o := new Owner(); "
    "    return await C.Inner(o.N); ");

BEZEL_TEST(codegen_async_chained_return_2731, expression_getter_control) {
    BEZEL_EXPECT_EQ(runPlain(kExprGetterControl), 1);
}
