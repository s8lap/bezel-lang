// Codegen — fix #3099: the anonymous-producer `.Result` read freed the task AT
// the read, not at the §28.3 K8 full-expression boundary. The exact twin of the
// `await` defect fixed in #3070, and the spec names the two in ONE clause.
//
// Spec (revision v0.252):
//   §28.3 "Unbound owned temporaries" —
//     "The borrowing uses include serving as the operand of a Pattern A `await`
//      **or `.Result` read** whose borrowed result is itself confined to the
//      same full expression (§30.5, §30.3) — for such an operand the disposal is
//      deferred to the end of the full expression rather than performed at the
//      await, so the borrowed result outlives its uses."
//   §28.3, same subsection — such a temporary "is disposed at the **end of the
//     full expression** in which it is produced, after every use of it within
//     that expression has completed", and on an exception "every already-
//     produced, not-yet-consumed unbound owned temporary is disposed during
//     unwinding … on the establishment-ordered cleanup stack".
//   §30.3 "The Task, Task<T> and OwningTask<T> types" —
//     "an unbound owned temporary (`SomeProducer().Result`) is admitted when the
//      borrow is confined to the same full expression, the temporary being
//      disposed at the expression's end **exactly as for the corresponding
//      `await`**."
//   §30.5 "The scoped await-bind", *`.Result` in lockstep* — "The admission
//     applies identically to the completed-task `.Result` read … exactly as
//     await and `.Result` have moved in lockstep since their temporary machinery
//     was unified."
//
// THE DEFECT. `lowerExprInner`'s `.Result` member-access path
// (LlvmBackend_Exprs.cpp) emitted `bz_task_free(task)` immediately after
// `CreateLoad(resTy, resPtr)` whenever the receiver was an anonymous producer
// call. Its in-code justification was that "the value is already in a register
// at that point so the freed buffer does not alias the returned val" — the same
// sentence #3070 refuted for `await`: true for an `int`, and FALSE for a Pattern
// A class result, where the loaded "value" IS the heap pointer and, since #1503,
// `bz_task_free` cascades into the per-`T` `result_disposer` and frees the
// pointee. #3074 had already made the STATIC admission structurally shared
// between `await` and `.Result`, so sema accepted these programs cleanly while
// codegen still freed at the read.
//
// OBSERVED PRE-FIX (bezelc --sanitize=address, HEAD 663a68e):
//     ERROR: AddressSanitizer: heap-use-after-free … READ of size 4
//       #0 bz$M.C$Use
//     freed by thread T0 here: bz$elemDispose      <- the #1503 result disposer
//     previously allocated by: bz$async$FetchAsync$Resume
// on the call-argument, member-access, two-reads and loop cases below, and
//     ERROR: AddressSanitizer: SEGV on unknown address 0x0c  in bz$M.C$Sum
// on the intrinsic-collection case.
//
// THE FIX. For a receiver whose task type carries a task-side result disposer
// (`patternAResultDisposerFor` — the one predicate that decides "disposing this
// task disposes its result", asked directly so the `await` and `.Result` sides
// cannot drift apart again), the reclaim is deferred to the K8 boundary through
// the same establishment/discharge pair §28.3's other unbound owned temporaries
// use: `registerAwaitedTaskTemp` at the read, `flushAwaitedTaskTemps` at
// `lowerExpr` depth 0. Every other row is untouched — a value / `string` /
// runtime-managed result, a BOUND receiver (`t.Result`, whose binder disposes
// it), `OwningTask<T>` (Pattern B), and the FAULT edge, where no result exists
// to borrow.
//
// THE SLOT INITIALIZATION is the one place this fix deliberately differs from
// #3070's: a `.Result` read is only legal in a SYNCHRONOUS body (§30.3 rejects
// it in async code), so its slot is not frame-promoted and nothing zeroes it.
// `not_taken_conditional_arm_*` below is the case that pins that — with #3070's
// uninitialized `entryAlloca` it SEGVs inside `free()` at the boundary flush.
//
// ORACLES. Every case runs on BOTH the plain and the ASan/LSan build (#2633:
// neither dominates), and each proves reclaim POSITIVELY through a live-object
// counter folded into the exit code, so a leak (`Cell.n > 0` / an LSan report)
// and a premature free (wrong value / crash) are each observable as a distinct
// exit code rather than as a silent pass.

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
// disposes the result. `Main` is SYNCHRONOUS — §30.3 admits `.Result` only
// outside async code.
std::string program(const std::string& mainBody) {
    return std::string("module M; ") + kCell +
        "class C "
        "  public static async method P(k: int): Task<Cell> "
        "    return new Cell(k); "
        "  end P; "
        "  public static async method PInt(k: int): Task<int> "
        "    return k; end PInt; "
        "  public static async method Boom(): Task<Cell> "
        "    throw new InvalidOperationException(\"boom\"); end Boom; "
        "  public static method Use(c: Cell): int return c.v; end Use; "
        "  public static method Use2(a: Cell, b: Cell): int "
        "    return a.v + b.v; end Use2; "
        "  public static method UseInt(a: Cell, b: int): int "
        "    return a.v + b; end UseInt; "
        "  public static method Throw(): int "
        "    throw new InvalidOperationException(\"boom\"); end Throw; "
        "  public static method AddInts(a: int, b: int): int "
        "    return a + b; end AddInts; "
        "  public static method False(): bool return false; end False; "
        "  public static method Main(): int " + mainBody +
        "  end Main; "
        "end C;";
}

// §30.3's own admitted shape: the borrowed result is consumed by a call within
// the same full expression. 42 = the borrow was live for the call; 92/93 = the
// task (and with it the result) was not reclaimed exactly once at the boundary.
const char* kCallArgument =
    "  var n := C.Use(C.P(42).Result); "
    "  if n <> 42 then return 91; end; "
    "  if Cell.n <> 0 then return 92; end; "
    "  if Cell.made <> 1 then return 93; end; "
    "  return 0; ";

// The member-access composition, admitted by the same clause.
const char* kMemberAccess =
    "  var n := C.P(42).Result.v; "
    "  if n <> 42 then return 91; end; "
    "  if Cell.n <> 0 then return 92; end; "
    "  if Cell.made <> 1 then return 93; end; "
    "  return 0; ";

// TWO anonymous-producer reads in ONE full expression: both borrows must be live
// for the call, and both tasks reclaimed at the single K8 boundary.
const char* kTwoReadsOneExpression =
    "  var n := C.Use2(C.P(40).Result, C.P(2).Result); "
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
    "    s := s + C.Use(C.P(14).Result); "
    "    if Cell.n <> 0 then return 94; end; "
    "    i := i + 1; "
    "  end; "
    "  if s <> 42 then return 91; end; "
    "  if Cell.n <> 0 then return 92; end; "
    "  if Cell.made <> 3 then return 93; end; "
    "  return 0; ";

// §28.3's EXCEPTION edge: a later operand of the same full expression throws
// AFTER the read produced its temporary. The deferred reclaim is established on
// the cleanup stack at the read, so the unwind disposes it — pre-fix this was
// covered by the eager free, and the fix must not trade the use-after-free for a
// leak (LSan is the oracle for that direction).
const char* kThrowLaterInSameExpression =
    "  var caught := 0; "
    "  try "
    "    var n := C.UseInt(C.P(40).Result, C.Throw()); "
    "    if n <> 0 then return 90; end; "
    "  catch var e: InvalidOperationException => caught := 1; "
    "  end; "
    "  if caught <> 1 then return 91; end; "
    "  if Cell.n <> 0 then return 92; end; "
    "  if Cell.made <> 1 then return 93; end; "
    "  return 0; ";

// The slot-initialization case. The read sits in a NOT-TAKEN conditional arm, so
// the deferred slot is never stored — yet the K8 boundary flush is emitted in the
// merge block and runs unconditionally. It must read NULL there. With #3070's
// uninitialized `entryAlloca` this SEGVs inside `free()`; nothing was ever
// constructed, so `Cell.made` must still be 0.
const char* kNotTakenConditionalArm =
    "  var n := (if C.False() then C.Use(C.P(42).Result) else 7); "
    "  if n <> 7 then return 91; end; "
    "  if Cell.n <> 0 then return 92; end; "
    "  if Cell.made <> 0 then return 93; end; "
    "  return 0; ";

// CONTROL — a BOUND receiver (`using var t := P(42); t.Result`) was correct
// before the fix and must stay correct after it: `isBoundTask` is true, so no
// reclaim happens at the read at all and the binder disposes the task.
const char* kBoundReceiverControl =
    "  do "
    "    using var t := C.P(42); "
    "    var n := C.Use(t.Result); "
    "    var m := t.Result.v; "
    "    if n <> 42 then return 91; end; "
    "    if m <> 42 then return 92; end; "
    "  end; "
    "  if Cell.n <> 0 then return 93; end; "
    "  if Cell.made <> 1 then return 94; end; "
    "  return 0; ";

// CONTROL — a VALUE-typed result (`Task<int>`). No borrow is produced, so this
// row keeps freeing at the read; the case pins that the fix did not widen its
// predicate past the class/collection row (a leak here would be an LSan failure).
const char* kValueResultControl =
    "  var n := C.AddInts(C.PInt(40).Result, 2); "
    "  if n <> 42 then return 91; end; "
    "  return 0; ";

// CONTROL — the FAULT edge. `.Result` on a faulted anonymous producer re-raises,
// and that path reclaims the task where it always did (no result exists there to
// borrow). Unchanged by the fix, and must not double-free with the deferred
// success-edge slot.
const char* kFaultEdgeControl =
    "  var caught := 0; "
    "  try "
    "    var n := C.Use(C.Boom().Result); "
    "    if n <> 0 then return 90; end; "
    "  catch var e: InvalidOperationException => caught := 1; "
    "  end; "
    "  if caught <> 1 then return 91; end; "
    "  if Cell.n <> 0 then return 92; end; "
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
    "  public static method Main(): int "
    "    var n := C.Sum(C.MakeArr().Result); "
    "    if n <> 6 then return 91; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

} // namespace

// §30.3's literal admitted shape. Pre-fix: heap-use-after-free under ASan.
BEZEL_TEST(result_unbound_task_k8_3099, call_argument_borrow_live_for_the_call_28_3) {
    const std::string src = program(kCallArgument);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// The member-access composition, admitted by the same §28.3 clause.
BEZEL_TEST(result_unbound_task_k8_3099, member_access_on_result_28_3) {
    const std::string src = program(kMemberAccess);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// Two anonymous-producer reads in one full expression.
BEZEL_TEST(result_unbound_task_k8_3099, two_reads_in_one_expression_28_3) {
    const std::string src = program(kTwoReadsOneExpression);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// K8.2: each evaluation is its own boundary — per-iteration reclaim, no
// accumulation across the loop.
BEZEL_TEST(result_unbound_task_k8_3099, loop_body_reclaims_per_iteration_28_3) {
    const std::string src = program(kLoopPerIteration);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// §28.3's exception-path discharge: the deferred temporary is reclaimed on the
// unwind edge, so the fix trades no use-after-free for a leak.
BEZEL_TEST(result_unbound_task_k8_3099, throw_later_in_same_expression_reclaims_28_3) {
    const std::string src = program(kThrowLaterInSameExpression);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// The deferred slot must read null on a path where the read never ran.
BEZEL_TEST(result_unbound_task_k8_3099, not_taken_conditional_arm_flushes_null_28_3) {
    const std::string src = program(kNotTakenConditionalArm);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// CONTROL: a bound receiver, correct before and after.
BEZEL_TEST(result_unbound_task_k8_3099, bound_receiver_unchanged_30_3) {
    const std::string src = program(kBoundReceiverControl);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// CONTROL: a value-typed result still frees at the read (no borrow, no leak).
BEZEL_TEST(result_unbound_task_k8_3099, value_typed_result_frees_at_read_30_3) {
    const std::string src = program(kValueResultControl);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// CONTROL: the fault edge reclaims where it always did.
BEZEL_TEST(result_unbound_task_k8_3099, fault_edge_reclaim_unchanged_30_9) {
    const std::string src = program(kFaultEdgeControl);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// An intrinsic-collection Pattern A result (#2279's disposer row).
BEZEL_TEST(result_unbound_task_k8_3099, collection_typed_result_28_3) {
    const std::string src = kCollectionResultProgram;
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}
