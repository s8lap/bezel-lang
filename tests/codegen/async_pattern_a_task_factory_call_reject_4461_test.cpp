// Codegen / soundness — issue #4461 (§30.5 "Result lifetime and task disposal",
// kind 1, v0.306): an EXPRESSION-bodied Pattern-A async method whose body is
// a bare (non-`await`) `Task.FromOwnedResult(new T())` call fell through
// `detectAsyncPatternAKind`'s top-level rejection set without diagnostic —
// the kind-1 result disposer was then attached to a result buffer holding a
// TASK pointer, NOT a T pointer. The first consumer's `await` read the Task
// pointer as a T and dereferenced it — a deterministic SIGSEGV on the plain
// build (exit 139 from WIFSIGNALED SIGSEGV #1 inside
// `bz_task_destroy`'s per-T disposer call).
//
// Spec (revision v0.306), §30.5 "Result lifetime and task disposal",
// *Producer-side return-type rules*:
//   "kind 1 — a `return` of an OWNED expression: the task owns the result
//    and its destructor disposes it directly."
//   "...kind 2 — `return await` over an owned Pattern A source — the
//    chained slot..."
//   "...kind 3 — `return await` over a borrowed source..."
// A bare (non-`await`) return must satisfy kind 1's parenthetical; the
// operand must be an OWNED expression. `Task.FromOwnedResult(...)` returns
// an `OwningTask<T>` (per §30.10) — not an owned `T`, and therefore
// cannot land in the result slot the kind-1 disposer reads as a T. The
// fix extends the top-level rejection set at `detectAsyncPatternAKind`
// (LlvmBackend_Async.cpp) to CallExpr over `Task.FromOwnedResult` and
// emits a §30.5 kind-1 violation diagnostic.
//
// MEASURED, this parent (cycle-13 L9 pre-#4461 codegen trace, on a
// reverted tree): `scripts/agent_check.sh --probe --asan` on the issue's
// exact reproducer: `==123==ERROR: AddressSanitizer: SEGV ...  #0
// bz$elemDispose ... in bz_task_destroy`. Exit 139. After the fix:
// bezelc rejects at compile time with a §30.5 diagnostic carrying a real
// source location.
//
// Companion defect #4462 — Pattern A `using var x := await t;` consumer
// was UAF'd by the producer-side kind-1 disposer attached to the SAME
// task's result slot. Closed by the same `detectAsyncPatternAKind`
// extension (the producer-side rejection makes the dangerous producer
// shape a compile-time error) and by an independent
// `lowerLocalDecl`+`lowerAwait` extension that clears the task's result
// slot for plain-`Task<T:class>` consuming bindings; covered separately
// by the control oracle rows below (the loop-amplified dispose-counter
// proof: 200 producers, 200 disposers, net 0 — a one-shot LSan row
// passes on BOTH pre- and post-fix because the live-count is still
// stack-reachable at exit).
//
// ORACLES. The rejection row uses `failedAtCompileTimeWithRealLocation`
// (a `CompileOutcome::Rejected` with a real source location, not a
// runtime crash/signal/timeout/ASan report — see
// tests/support/aot_result_asserts.hpp:113) and pins the diagnostic's
// unique fragment. The control rows pin that (a) the corrected
// `=> await Task.FromOwnedResult(new T())` form compiles AND runs
// cleanly on both plain and ASan oracles (#2633: neither dominates), and
// (b) the loop-amplified consumer `using var x := await t` shape is
// leak+UAF clean (200 ctors == 200 dtors, no SEGV, no ASan report).
//
// Note on stdlib siblings: `Task.FromResult<T>` rejects class T at the
// stdlib boundary per §30.10 ("For class T, use FromOwnedResult
// instead; a move-only value T is rejected per spec §30.5") and
// `Task.FromException<T>` takes an `Exception`, not a T. Neither
// rejection fires through `detectAsyncPatternAKind`'s new CallExpr arm,
// so they have no separate test row here — closing the stdlib name list
// (only `FromOwnedResult` is reachable for the class-T wrong-slot shape
// that *is* the actual reproducer).
//
// fix #4670 / §30.5 "Consumer-side rules" (v0.307, line 8551): the two
// ACCEPT rows below (`accept_awaited_exprbody_task_fromownedresult_30_5`,
// `accept_blockbody_task_fromownedresult_30_5`) originally consumed `t:
// Task<Widget4461>` (a Pattern A borrow-producing await — Widget4461 is a
// class) with `using var w := await t;`. #4670 added the correct §30.5
// rejection for exactly that shape ("the borrow produced by `await
// Task<T>` (Pattern A) may not flow to a `using var` binding") but did not
// touch these two rows, which predate it — so both started failing to
// compile (`r.ran == false`) rather than exercising the loop-amplified
// ctor/dtor oracle they exist to run. Both rows are corrected here to the
// spec-correct consumer form, `var w := await t;` (§30.5's own "canonical"
// discharge for a Pattern A borrow local) — the ctor/dtor-count assertions
// are unchanged and remain exactly as strict. `reject_using_var_await_...`
// below pins the OLD wrong shape as a compile rejection so the two
// checks (#4461's producer-side kind-1 rejection above, and #4670's
// consumer-side `using var` rejection) cannot silently drift apart again.

#include <string>

#include "support/aot_result_asserts.hpp"   // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"          // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The unique text that ONLY this fix's diagnostic carries (per the
// `detectAsyncPatternAKind` arm added in LlvmBackend_Async.cpp).
// Pinning it prevents any future "sounds-like-§30.5" diagnostic sharing
// the same prefix from silently satisfying this row.
constexpr const char* kFixDiagFragment =
    "§30.5 Pattern A) may only `return` an OWNED expression";

// fix #4670 / §30.5 (v0.307): the unique text ONLY the consumer-side
// `using var` await-borrow rejection carries. Distinct from
// `kFixDiagFragment` above (a different check, in a different file/pass —
// #4461's producer-side kind-1 rejection vs #4670's consumer-side
// `using var`-binding rejection).
constexpr const char* kUsingVarBorrowDiagFragment =
    "the borrow produced by `await Task<T>` (Pattern A) may not flow to a "
    "`using var` binding";

constexpr int kLoopIters = 200;

constexpr const char* kWidget =
    "class Widget4461 "
    "  public static ctorCount: int := 0; "
    "  public static dtorCount: int := 0; "
    "  public constructor() Widget4461.ctorCount := Widget4461.ctorCount + 1; "
    "    end constructor; "
    "  destructor Widget4461.dtorCount := Widget4461.dtorCount + 1; "
    "    end destructor; "
    "end Widget4461; ";

// Body under test (#4461 reproducer). The same wrong-slot shape on a
// method declaration is rejected with this fix's diagnostic (covered by
// `reject_exprbody_task_fromownedresult_30_5` below); the lambda form
// is exercised here because the loop-amplified runtime proof below
// needs the `() -> Task<Widget4461>` typing reachable as a `var f :=`
// target.
constexpr const char* kExprBodyAwaited =
    "async lambda() => await Task.FromOwnedResult(new Widget4461())";

// Block-bodied twin.
constexpr const char* kBlockBody =
    "async lambda() => do "
    "          return await Task.FromOwnedResult(new Widget4461()); "
    "        end";

}  // namespace

// The issue's exact reproducer (minimized — no exit payload needed; the
// compile-time rejection alone establishes the fix).
BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          reject_exprbody_task_fromownedresult_30_5) {
    // The arrow-body must NOT have an `end Make;` trailer — §17 grammar only
    // admits `end <name>` for block bodies; arrow bodies terminate at the
    // `;`. The parser emitted a different "end label does not match…" error
    // before the fix's reach, which the negative-keyword assertion below
    // would silently MASK — closing that confusion mode is the very reason
    // this row uses a `#failedAtCompileTimeWithRealLocation` predicate
    // (real path:line:col) plus a diagnostic-fragment pin, and not just
    // `!r.ran`.
    const std::string src =
        std::string("module M; ") + kWidget +
        "class C "
        "  public static async method Make(): Task<Widget4461> "
        "    => Task.FromOwnedResult(new Widget4461()); "
        "  public static async method Main(): Task<int> "
        "    using var t := C.Make(); "
        "    var w := await t; "
        "    return Widget4461.ctorCount; "
        "  end Main; "
        "end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

// ACCEPT-SIDE CONTROL — the corrected form (`=> await Task.FromOwnedResult(...)`)
// must compile and run cleanly under BOTH plain and ASan oracles (#2633).
// Pins that the rejection is narrowly scoped to the bare-return shape and
// does NOT spuriously reject the well-typed `await`-prefixed twin.
BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          accept_awaited_exprbody_task_fromownedresult_30_5) {
    const std::string lambda = kExprBodyAwaited;
    const std::string src =
        std::string("module M; ") + kWidget +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var sum: int := 0; "
        "    var i: int := 0; "
        "    while i < " + std::to_string(kLoopIters) + " do "
        "      do "
        "        var f: () -> Task<Widget4461> := " + lambda + "; "
        "        using var t := f(); "
        // fix #4670 / §30.5 (v0.307): `t: Task<Widget4461>` (class T) is a
        // Pattern A borrow-producing await source; the borrow `await t`
        // produces may not flow to a `using var` binding (only to a plain
        // `var`, per §30.5's consumer-side rules) — `using var w := await
        // t;` used to be accepted here (pre-#4670) but is now a compile
        // error (see `reject_using_var_await_task_pattern_a_borrow_30_5`
        // below). `var w` is the spec-correct discharge; `t` remains the
        // `using var` that owns the disposal.
        "        var w := await t; "
        "        i := i + 1; "
        "      end; "
        "    end while; "
        "    if Widget4461.ctorCount = " + std::to_string(kLoopIters) +
        "      and Widget4461.dtorCount = " + std::to_string(kLoopIters) +
        "      then return 42; end; "
        "    if Widget4461.ctorCount <> " + std::to_string(kLoopIters) +
        "      then return 91; end; "
        "    if Widget4461.dtorCount <> " + std::to_string(kLoopIters) +
        "      then return 92; end; "
        "    return 99; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
    JitResult rc = runAotChecked(src);
    if (rc.ran) {
        // The PLAIN oracle is the strict one — `rc.ran == true` means ASan
        // linked and the program actually ran under it. If ASan is
        // unavailable on this box, the harness returns `rc.ran == false`
        // AND a `SKIP` diagnostic; that is the loud #3733 failure mode,
        // asserted in `check_toolchain_skip_surface`'s `absorbing_sites`
        // ratchet (its `checked_calls ROSE` arm — a new `runAotChecked`
        // call site is admitted IFF the upstream baseline accommodates
        // it; here the rise is banked alongside the bugfix rather than
        // silently absorbed behind a `wasSkipped(...)? - : assert` guard
        // (the silent-skip defect #3733 fix #1 bans).
        BEZEL_EXPECT_EQ(rc.exitCode, 42);
    }
}

// REJECT-SIDE — fix #4670 / §30.5 (v0.307): pins the OLD wrong shape (the
// consumer form the two accept rows above used before #4670 landed) as a
// compile-time rejection, so a future regression to either check cannot
// silently re-admit it without turning a test red. `t: Task<Widget4461>`
// is a Pattern A borrow source (Widget4461 is a class); `using var w :=
// await t;` cannot discharge a borrow's disposal obligation it does not
// own.
BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          reject_using_var_await_task_pattern_a_borrow_30_5) {
    const std::string lambda = kExprBodyAwaited;
    const std::string src =
        std::string("module M; ") + kWidget +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var f: () -> Task<Widget4461> := " + lambda + "; "
        "    using var t := f(); "
        "    using var w := await t; "
        "    return Widget4461.ctorCount; "
        "  end Main; "
        "end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kUsingVarBorrowDiagFragment));
}

// CONTROL — the already-working BLOCK-bodied twin (the call site the
// issue's own text identifies as not regressed) must remain accepted.
// Pinned identically on both oracles.
BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          accept_blockbody_task_fromownedresult_30_5) {
    const std::string lambda = kBlockBody;
    const std::string src =
        std::string("module M; ") + kWidget +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var sum: int := 0; "
        "    var i: int := 0; "
        "    while i < " + std::to_string(kLoopIters) + " do "
        "      do "
        "        var f: () -> Task<Widget4461> := " + lambda + "; "
        "        using var t := f(); "
        // fix #4670 / §30.5 (v0.307): see the identical note on the
        // expr-body accept row above — `using var w := await t;` (t:
        // Task<Widget4461>, Pattern A) is rejected post-#4670; `var w` is
        // the spec-correct borrow-local discharge.
        "        var w := await t; "
        "        i := i + 1; "
        "      end; "
        "    end while; "
        "    if Widget4461.ctorCount = " + std::to_string(kLoopIters) +
        "      and Widget4461.dtorCount = " + std::to_string(kLoopIters) +
        "      then return 42; end; "
        "    if Widget4461.ctorCount <> " + std::to_string(kLoopIters) +
        "      then return 91; end; "
        "    if Widget4461.dtorCount <> " + std::to_string(kLoopIters) +
        "      then return 92; end; "
        "    return 99; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
    JitResult rc = runAotChecked(src);
    if (rc.ran) {
        BEZEL_EXPECT_EQ(rc.exitCode, 42);
    }
}

// ===========================================================================
// fix-forward #6405 — the `=> E` arrow-body carve-out must be as NARROW as
// the rule it defers to, and the rule is Pattern A ONLY.
//
// 2425187c43 ("fix #6405: let §30.5 own the async producer arrow body it is
// written for") suppressed §5.1 in the EXPRESSION-BODY arm of
// `runMethodBody` so that §30.5's Pattern A producer-side check would lead
// instead. It gated the carve-out on four conjuncts, the fourth being
// `isTaskShapedRef(ctx_.exprType(*exprBody))` — a predicate that admits the
// WHOLE task family, `OwningTask<T>` included. The rule it defers to is
// scoped in the spec's own words to Pattern A:
//
//   §30.5 "Result lifetime and task disposal", *Producer-side return-type
//   rules*, the bolded clause:
//     "**No fourth kind: a non-`await` borrow return is rejected.** A
//      `return` in an `async` body — an `async` method's or an `async`
//      lambda's — whose return type is `Task<T>` for class or interface T,
//      whose operand is a **borrow** and not a `return await` ... is a
//      **compile error at the return site**".
//
//   The `OwningTask<T>` row is a SIBLING of that row and carries no such
//   clause: "`OwningTask<T>` for **class or interface** T: the task produces
//   a single owned result. Awaiting transfers ownership to the awaiter ...
//   This is **Pattern B**." There is no "fourth kind" prohibition at a
//   Pattern B producer, so there is nothing for §30.5 to own there — and,
//   worse, nothing behind the suppression to reject the program either:
//   `detectAsyncPatternAKind` (LlvmBackend_Async.cpp) reports §30.5 kind 1
//   only under a non-null `asyncPatternAResultDisposer_`, which is attached
//   for Pattern A only. A Pattern B producer therefore reaches codegen AND
//   RUNS.
//
// MEASURED, both legs, my own numbers (not quoted from the review):
//
//   `async method Make(): OwningTask<Widget> => Task.FromOwnedResult(new
//    Widget());`  — a 200-iteration program awaiting it in a loop.
//
//     2425187c43 (picked)  bezelc rc=0 (ACCEPTED)
//                          run x3 -> exit 139 x3  (SIGSEGV, all three)
//                          under --sanitize=address:
//                            "ERROR: AddressSanitizer: SEGV on unknown
//                             address 0x000000000010 ... The signal is
//                             caused by a READ memory access.
//                               #0 ... in bz$async$M.C$Main$Resume"
//     2425187c43^ (base)  bezelc rc=1, §5.1
//     this branch         bezelc rc=1, §5.1
//
//   The same spelling with `await` — the remedy the compiler's own §5.1
//   message names — exits 42 cleanly on all three legs, plain and ASan.
//
//   The STATEMENT arm of the identical method already rejected this shape
//   with §30.6 on all three legs, naming `OwningTask<T>` in its own text
//   ("an `async` method's `return` must produce the element type of its
//   `Task<T>`/`OwningTask<T>` return, not a `Task` value; add `await`").
//   An arrow arm that ACCEPTS what the statement arm REJECTS on the same
//   method is the signature of a hole, not of a hand-off.
//
// ORACLE. `reject_owningtask_exprbody_task_fromownedresult_30_5` below is
// the pin that matters more than the code change: on 2425187c43 the program
// COMPILES, so `failedAtCompileTimeWithRealLocation` is false and the row is
// RED; with the fix it rejects with a real source location and the §5.1
// needle, and it is GREEN. It is not vacuous — `runAot` fills
// `JitResult::diagnostics` only on a reject, so an accepting program leaves
// the assertion genuinely false rather than silently skipped.
//
// THE MATRIX. The predecessor's probe matrix enumerated
// element-type x arm but never crossed the DECLARED WRAPPER, which is why
// this landed: `Task<T>` and `OwningTask<T>` were not distinguished anywhere.
// The rows below are the full 2 x 4 x 2 grid as individual named tests, so
// no future cell can be missing the way this one was. Grid:
//   wrapper in {Task, OwningTask} x element in {class, interface, Object,
//   string} x arm in {arrow, statement-block}, plus a non-factory task call
//   and a `FromException` control.
//
// Two cells of that grid — `OwningTask<interface>` arrow and
// `OwningTask<Object>` arrow — are ACCEPTED, but they were accepted on
// 2425187c^ too (MEASURED: leg B rc=0) and are STILL accepted with this
// fix: a different mechanism, not the carve-out. §5.1's built-in-class
// convertibility path does not fire for an interface/`Object` target at
// all, so no suppression is involved and none is removed. The same program
// segfaults 3/3 the same way. No row below asserts either verdict for those
// two cells: pinning the acceptance would bank a known defect as correct,
// and pinning a rejection would be red on this branch. Both are reported
// and filed instead.

namespace fixforward6405 {

constexpr const char* kPrelude =
    "module M; "
    "public interface IShape method Area(): int; end IShape; "
    "public class Sq: IShape public method Area(): int => 4; end Sq; "
    "public class Widget public method Area(): int => 7; end Widget; "
    "public class MyEx: Exception end MyEx; ";

// §5.1's own convertibility sentence — the diagnostic a Pattern B arrow
// body is restored to, and the one 2425187c^ produced for this shape.
constexpr const char* kNotConvertible51 =
    "not implicitly convertible to the target type";

// §30.6's forgotten-`await` message — the STATEMENT arm's verdict on the
// identical method. Distinct from `kNotConvertible51` (a different check, in
// a different arm) and from `kFixDiagFragment` (Pattern A, codegen).
constexpr const char* kAddAwait306 =
    "must produce the element type of its `Task<T>`/`OwningTask<T>` return";

// The stdlib-boundary rejection on `OwningTask<string>` (§30.5's
// `OwningTask<T>` rows admit class or interface T only).
constexpr const char* kOwningTaskNeedsClass =
    "`OwningTask` requires a class or interface element type";

// (wrapper, element) -> the factory expression producing that element.
// `FromOwnedResult` is the reachable class-T factory (stdlib §6.7:
// `Task.FromResult<T>` rejects class T, `Task.FromException<T>` takes an
// `Exception`, so only `FromOwnedResult` forms the class-T wrong-slot
// shape); `string` cannot be `owned`, so it uses `FromResult`.
struct Cell {
    const char* ret;    // the DECLARED async return type
    const char* expr;   // the arrow/statement body operand
};

constexpr Cell kCells[] = {
    {"Task<Widget>",      "Task.FromOwnedResult(new Widget())"},
    {"Task<IShape>",      "Task.FromOwnedResult(new Sq())"},
    {"Task<Object>",      "Task.FromOwnedResult(new Widget())"},
    {"Task<string>",      "Task.FromResult(\"hi\")"},
    {"OwningTask<Widget>", "Task.FromOwnedResult(new Widget())"},
    {"OwningTask<IShape>", "Task.FromOwnedResult(new Sq())"},
    {"OwningTask<Object>", "Task.FromOwnedResult(new Widget())"},
    {"OwningTask<string>", "Task.FromResult(\"hi\")"},
};

// Both arms of one grid cell.
inline std::string buildMethod(const Cell& c, bool arrow) {
    return arrow ? std::string("  public static async method Make(): ") +
                       c.ret + " => " + c.expr + "; "
                 : std::string("  public static async method Make(): ") +
                       c.ret + " return " + c.expr + "; end Make; ";
}

inline std::string buildProgram(const Cell& c, bool arrow) {
    return std::string(kPrelude) + "public class C " + buildMethod(c, arrow) +
           "  public static method Main(): int => 0; "
           "end C;";
}

inline const Cell& cell(size_t i) { return kCells[i]; }

}  // namespace fixforward6405

// ---------------------------------------------------------------------------
// THE PIN. `OwningTask<Widget>` element + `Task.FromOwnedResult` factory call
// + `=>` arrow body => a REJECTION. This is the shape the predecessor's
// matrix never enumerated.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          reject_owningtask_exprbody_task_fromownedresult_30_5) {
    const auto& c = fixforward6405::cell(4);  // OwningTask<Widget>
    auto r = runAot(fixforward6405::buildProgram(c, /*arrow=*/true));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, fixforward6405::kNotConvertible51));
}

// The STATEMENT arm of the same method, unchanged on every leg. It is the
// evidence that the two arms must agree on the verdict, and §30.6's own text
// names `OwningTask<T>` — the section that says a Pattern B producer must
// still produce the ELEMENT type.
BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          reject_owningtask_blockbody_task_fromownedresult_30_6) {
    const auto& c = fixforward6405::cell(4);  // OwningTask<Widget>
    auto r = runAot(fixforward6405::buildProgram(c, /*arrow=*/false));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, fixforward6405::kAddAwait306));
}

// ---------------------------------------------------------------------------
// THE MATRIX, row by row. Every cell that REJECTS, pinned by section.
// ---------------------------------------------------------------------------

// --- Pattern A (`Task<T>`), ARROW arm: §30.5 must still LEAD (the
// predecessor's fix-forward, preserved unchanged by this commit). ---
BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          matrix_task_class_exprbody_30_5) {
    const auto& c = fixforward6405::cell(0);
    auto r = runAot(fixforward6405::buildProgram(c, true));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          matrix_task_iface_exprbody_30_5) {
    const auto& c = fixforward6405::cell(1);
    auto r = runAot(fixforward6405::buildProgram(c, true));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          matrix_task_object_exprbody_30_5) {
    const auto& c = fixforward6405::cell(2);
    auto r = runAot(fixforward6405::buildProgram(c, true));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

// `Task<string>` has NO per-result disposer, so `asyncPatternAResultDisposer_`
// is null and Pattern A's producer-side rejection does not fire for it either.
// §5.1 must keep the position — the predecessor measured this and excluded
// `string`; this row pins the exclusion so it cannot silently widen.
BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          matrix_task_string_exprbody_5_1) {
    const auto& c = fixforward6405::cell(3);
    auto r = runAot(fixforward6405::buildProgram(c, true));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, fixforward6405::kNotConvertible51));
}

// --- Pattern A (`Task<T>`), STATEMENT arm: §30.6 on all four elements. ---
BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          matrix_task_class_blockbody_30_6) {
    const auto& c = fixforward6405::cell(0);
    auto r = runAot(fixforward6405::buildProgram(c, false));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, fixforward6405::kAddAwait306));
}

BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          matrix_task_iface_blockbody_30_6) {
    const auto& c = fixforward6405::cell(1);
    auto r = runAot(fixforward6405::buildProgram(c, false));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, fixforward6405::kAddAwait306));
}

BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          matrix_task_object_blockbody_30_6) {
    const auto& c = fixforward6405::cell(2);
    auto r = runAot(fixforward6405::buildProgram(c, false));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, fixforward6405::kAddAwait306));
}

BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          matrix_task_string_blockbody_30_6) {
    const auto& c = fixforward6405::cell(3);
    auto r = runAot(fixforward6405::buildProgram(c, false));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, fixforward6405::kAddAwait306));
}

// --- Pattern B (`OwningTask<T>`), ARROW arm. `class` is the pin above;
// `string` never reaches this call site (§30.5's `OwningTask<T>` rows admit
// class or interface T only, so the stdlib boundary rejects the declaration).
// `interface` and `Object` are the two ACCEPTED, PRE-EXISTING cells: accepted
// on 2425187c^, accepted still, segfaulting 3/3. Deliberately NOT pinned
// here in either direction — see the header note. ---
BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          matrix_owningtask_string_exprbody_5_1) {
    const auto& c = fixforward6405::cell(7);
    auto r = runAot(fixforward6405::buildProgram(c, true));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r,
                                               fixforward6405::kOwningTaskNeedsClass));
}

// --- Pattern B (`OwningTask<T>`), STATEMENT arm: §30.6 on all four. ---
BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          matrix_owningtask_class_blockbody_30_6) {
    const auto& c = fixforward6405::cell(4);
    auto r = runAot(fixforward6405::buildProgram(c, false));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, fixforward6405::kAddAwait306));
}

BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          matrix_owningtask_iface_blockbody_30_6) {
    const auto& c = fixforward6405::cell(5);
    auto r = runAot(fixforward6405::buildProgram(c, false));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, fixforward6405::kAddAwait306));
}

BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          matrix_owningtask_object_blockbody_30_6) {
    const auto& c = fixforward6405::cell(6);
    auto r = runAot(fixforward6405::buildProgram(c, false));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, fixforward6405::kAddAwait306));
}

BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          matrix_owningtask_string_blockbody_5_1) {
    const auto& c = fixforward6405::cell(7);
    auto r = runAot(fixforward6405::buildProgram(c, false));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r,
                                               fixforward6405::kOwningTaskNeedsClass));
}

// ---------------------------------------------------------------------------
// CONTROLS — the carve-out is not a blanket arrow-body exemption.
// ---------------------------------------------------------------------------

// A Task-returning call that is NOT a `Task` static-factory member is not in
// codegen's `isTaskFactoryCallExpr` set, so codegen does not reject it and
// §5.1 must keep the position.
BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          matrix_control_nonfactory_task_call_5_1) {
    auto r = runAot(std::string(fixforward6405::kPrelude) +
        "public class C "
        "  public static method Mk(): OwningTask<Widget> "
        "    => Task.FromOwnedResult(new Widget()); "
        "  public static async method Make(): Task<Widget> => Mk(); "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, fixforward6405::kNotConvertible51));
}

// `Task.FromException` is in the factory set but takes no result value, so
// over a value-typed `Task<int>` return the element check keeps §5.1.
BEZEL_TEST(async_pattern_a_task_factory_call_reject_4461,
          matrix_control_fromexception_value_task_5_1) {
    auto r = runAot(std::string(fixforward6405::kPrelude) +
        "public class C "
        "  public static async method Make(): Task<int> "
        "    => Task.FromException(new MyEx()); "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, fixforward6405::kNotConvertible51));
}
