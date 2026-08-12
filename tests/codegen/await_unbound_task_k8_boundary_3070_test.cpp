// Codegen — fix #3070: the anonymous awaited Pattern A task was freed AT the
// await, not at the §28.3 K8 full-expression boundary.
//
// Spec (revision v0.249):
//   §30.5 "Result lifetime and task disposal", Consumer-side rules, Pattern A —
//     "An **unbound owned temporary** (§28.3) — a bare producer call or other
//      unnamed owned task expression — bounds the borrow by its K8
//      full-expression boundary, *provided the borrowed result is used only
//      within that expression*: the temporary is then disposed at the
//      expression's end, **after every use of the borrow**, and disposing the
//      task disposes the borrowed result with it."
//   §30.5, same subsection —
//     "`Process(await Producer())` is therefore **admitted**: … the task — and
//      with it the result — is disposed at the end of the statement (§28.3);
//      **the disposal runs after the borrowing use**, so the result is live for
//      the call".
//   §28.3 "Unbound owned temporaries" — the temporary "is disposed at the end of
//     the full expression in which it is produced, after every use of it within
//     that expression has completed", and on an exception "every already-
//     produced, not-yet-consumed unbound owned temporary is disposed during
//     unwinding … on the establishment-ordered cleanup stack".
//
// THE DEFECT. `emitAwaitSuspend` (LlvmBackend_Async.cpp) emitted
// `bz_task_free(awaitedTask)` immediately after loading the await result
// whenever the operand was an anonymous producer call. Its in-code
// justification was that "the result is loaded BY VALUE into the await site
// before the task is freed below, so the value … do not alias afterwards" —
// true for an `int`, and FALSE for a Pattern A class result, where the loaded
// "value" IS the heap pointer and, since #1503, `bz_task_free` cascades into the
// per-`T` `result_disposer` and frees the pointee. Both shapes §30.5 names as
// admitted therefore read freed memory.
//
// OBSERVED PRE-FIX (bezelc --sanitize=address, HEAD 309356e), on the two
// programs below:
//     ERROR: AddressSanitizer: heap-use-after-free … READ of size 4
//       #0 bz$M.C$Use
//       #1 bz$async$Main$Resume
//     freed by thread T0 here: bz$elemDispose        <- the #1503 result disposer
//     previously allocated by:  bz$async$P$Resume
// and, on the plain oracle, nondeterministic garbage exit codes (201 169 118 …)
// where 42 was expected. The `Task<Array<int>>` case below SEGVs pre-fix.
//
// THE FIX. For a Pattern A operand whose result type carries a task-side
// disposer (`patternAResultDisposerFor` — the one predicate that decides
// "disposing this task disposes its result"), the reclaim is deferred to the K8
// full-expression boundary through the same establishment/discharge pair §28.3's
// other unbound owned temporaries already use (`registerAwaitedTaskTemp` at the
// await's success edge, `flushAwaitedTaskTemps` at `lowerExpr` depth 0). Every
// other row — value, `string`, runtime-managed, and the fault edge, where no
// result exists to borrow — keeps freeing at the await.
//
// ORACLES. Each case is run on BOTH the plain and the ASan/LSan build (#2633:
// neither dominates), and each proves reclaim POSITIVELY through a live-object
// counter folded into the exit code, so a leak (`Cell.n > 0`) and a premature
// free (wrong value / crash) are each observable as a distinct exit code rather
// than as a silent pass.

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

// Plain (non-instrumented) AOT run. A SIGSEGV is WIFSIGNALED -> ran=false -> -999.
int runPlain(const std::string& src) {
    JitResult r = bezel::test::runAot(src);
    return r.ran ? r.exitCode : -999;
}

// ASan+LSan run: a heap-use-after-free / double-free / leak surfaces as -999.
int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return -1000;   // ASan toolchain absent — case skipped
    return r.ran ? r.exitCode : -999;
}

// A disposal-counted class: `n` is the live count, `made` the construction
// count. Both folded into the exit code make "disposed exactly once, and not
// before the borrowing use" observable as a VALUE.
const char* kCell =
    "class Cell "
    "  public static n: int := 0; "
    "  public static made: int := 0; "
    "  public v: int; "
    "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; "
    "    Cell.made := Cell.made + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";

// `P` is the §30.5 Pattern A producer (`Task<T>` for class T): its non-chained
// `return <owned>` makes the task the result's owner (kind 1), so `bz_task_free`
// disposes the result.
std::string program(const std::string& mainBody) {
    return std::string("module M; ") + kCell +
        "class C "
        "  public static async method P(k: int): Task<Cell> "
        "    return new Cell(k); "
        "  end P; "
        "  public static method Use(c: Cell): int return c.v; end Use; "
        "  public static method Use2(a: Cell, b: Cell): int "
        "    return a.v + b.v; end Use2; "
        "  public static method UseInt(a: Cell, b: int): int "
        "    return a.v + b; end UseInt; "
        "  public static method Boom(): int "
        "    throw new InvalidOperationException(\"boom\"); end Boom; "
        "  public static async method PInt(k: int): Task<int> "
        "    return k; end PInt; "
        "  public static method AddInts(a: int, b: int): int "
        "    return a + b; end AddInts; "
        "  public static async method Main(): Task<int> " + mainBody +
        "  end Main; "
        "end C;";
}

// §30.5's own admitted shape: the borrowed result is consumed by a call within
// the same full expression. 42 = the borrow was live for the call; 92/93 = the
// task (and with it the result) was not reclaimed exactly once at the boundary.
const char* kCallArgument =
    "  var n := C.Use(await C.P(42)); "
    "  if n <> 42 then return 91; end; "
    "  if Cell.n <> 0 then return 92; end; "
    "  if Cell.made <> 1 then return 93; end; "
    "  return 0; ";

// The member-access composition §30.5 admits under the same clause.
const char* kMemberAccess =
    "  var n := (await C.P(42)).v; "
    "  if n <> 42 then return 91; end; "
    "  if Cell.n <> 0 then return 92; end; "
    "  if Cell.made <> 1 then return 93; end; "
    "  return 0; ";

// TWO awaits in ONE full expression. The first task's slot is written before the
// SECOND suspend and read at the boundary after it, so this is the case that
// needs the deferred slot to survive a resume on another thread's stack (§30.13
// — the slot joins `asyncHandlerSpillSlots_` and is frame-backed).
const char* kTwoAwaitsOneExpression =
    "  var n := C.Use2(await C.P(40), await C.P(2)); "
    "  if n <> 42 then return 91; end; "
    "  if Cell.n <> 0 then return 92; end; "
    "  if Cell.made <> 2 then return 93; end; "
    "  return 0; ";

// A LOOP body: §28.3's boundary is per-evaluation, so each iteration reclaims
// its own task and nothing accumulates. The in-loop `Cell.n` assertion pins the
// reclaim to the END of each iteration's full expression, not to scope exit.
const char* kLoopPerIteration =
    "  var s := 0; var i := 0; "
    "  while i < 3 do "
    "    s := s + C.Use(await C.P(14)); "
    "    if Cell.n <> 0 then return 94; end; "
    "    i := i + 1; "
    "  end; "
    "  if s <> 42 then return 91; end; "
    "  if Cell.n <> 0 then return 92; end; "
    "  if Cell.made <> 3 then return 93; end; "
    "  return 0; ";

// §28.3's EXCEPTION edge: a later operand of the same full expression throws
// AFTER the await produced its temporary. The deferred reclaim is established on
// the cleanup stack at the await, so the unwind disposes it — pre-fix this was
// covered by the eager free, and the fix must not trade the use-after-free for a
// leak (LSan is the oracle for that direction).
const char* kThrowAfterAwaitInSameExpression =
    "  var caught := 0; "
    "  try "
    "    var n := C.UseInt(await C.P(40), C.Boom()); "
    "    if n <> 0 then return 90; end; "
    "  catch var e: InvalidOperationException => caught := 1; "
    "  end; "
    "  if caught <> 1 then return 91; end; "
    "  if Cell.n <> 0 then return 92; end; "
    "  if Cell.made <> 1 then return 93; end; "
    "  return 0; ";

// CONTROL — the NAMED-operand form (`using var t := …; await t`) was correct
// before the fix and must stay byte-for-byte correct after it: the task is bound,
// so `freeTaskAfter` is false and no deferral is involved.
const char* kNamedOperandControl =
    "  do "
    "    using var t := C.P(42); "
    "    var n := C.Use(await t); "
    "    var m := (await t).v; "
    "    if n <> 42 then return 91; end; "
    "    if m <> 42 then return 92; end; "
    "  end; "
    "  if Cell.n <> 0 then return 93; end; "
    "  if Cell.made <> 1 then return 94; end; "
    "  return 0; ";

// CONTROL — a VALUE-typed result (`Task<int>`). No borrow is produced, so this
// row keeps freeing at the await; the case pins that the fix did not widen its
// predicate past the class/collection row (a leak here would be an LSan failure).
const char* kValueResultControl =
    "  var n := C.AddInts(await C.PInt(40), 2); "
    "  if n <> 42 then return 91; end; "
    "  return 0; ";

// A `Task<T>` whose T is an INTRINSIC COLLECTION (§30.5 / #2279): the task owns
// the backing storage through `patternAResultCollectionDisposer`, so the same
// cascade applies and the same boundary must hold. Pre-fix this SEGVs.
const char* kCollectionResultProgram =
    "module M; "
    "class C "
    "  public static async method MakeArr(): Task<Array<int>> "
    "    var a: Array<int> := [1, 2, 3]; "
    "    return a; "
    "  end MakeArr; "
    "  public static method Sum(a: Array<int>): int "
    "    var t := 0; "
    "    for var x in a do t := t + x; end; "
    "    return t; "
    "  end Sum; "
    "  public static async method Main(): Task<int> "
    "    var n := C.Sum(await C.MakeArr()); "
    "    if n <> 6 then return 91; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

} // namespace

// §30.5's literal example. Pre-fix: heap-use-after-free under ASan and a
// nondeterministic garbage exit code on the plain build.
BEZEL_TEST(await_unbound_task_k8_3070, call_argument_borrow_live_for_the_call_30_5) {
    const std::string src = program(kCallArgument);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// The member-access composition, admitted by the same §30.5 clause.
BEZEL_TEST(await_unbound_task_k8_3070, member_access_on_await_result_30_5) {
    const std::string src = program(kMemberAccess);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// Two awaits in one full expression — the deferred slot must survive the second
// suspend (§30.13 frame spill).
BEZEL_TEST(await_unbound_task_k8_3070, two_awaits_in_one_expression_30_5) {
    const std::string src = program(kTwoAwaitsOneExpression);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// K8.2: each evaluation is its own boundary — per-iteration reclaim, no
// accumulation across the loop.
BEZEL_TEST(await_unbound_task_k8_3070, loop_body_reclaims_per_iteration_28_3) {
    const std::string src = program(kLoopPerIteration);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// §28.3's exception-path discharge: the deferred temporary is reclaimed on the
// unwind edge, so the fix trades no use-after-free for a leak.
BEZEL_TEST(await_unbound_task_k8_3070, throw_later_in_same_expression_reclaims_28_3) {
    const std::string src = program(kThrowAfterAwaitInSameExpression);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// CONTROL: the named-operand form, correct before and after.
BEZEL_TEST(await_unbound_task_k8_3070, named_operand_form_unchanged_30_5) {
    const std::string src = program(kNamedOperandControl);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// CONTROL: a value-typed result still frees at the await (no borrow, no leak).
BEZEL_TEST(await_unbound_task_k8_3070, value_typed_result_frees_at_await_30_5) {
    const std::string src = program(kValueResultControl);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// An intrinsic-collection Pattern A result (#2279's disposer row).
BEZEL_TEST(await_unbound_task_k8_3070, collection_typed_result_30_5) {
    const std::string src = kCollectionResultProgram;
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}
