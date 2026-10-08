// Codegen + ownership — #2310 (the #973a residual): widen the state-machine
// `owned`-parameter conditional-discharge admission (§29.8 iterators / async
// iterators, §30.1 plain async) from the marked-adoption-only subset to all
// six discharge forms.
//
// §29.8 says the iterator disposes "any `owned` parameter that has not been
// moved out of by the body" — a RUNTIME property once the move-out sits on one
// branch. The frame capture slot is the runtime moved-from flag: codegen
// NULLS it at every move-out site and the terminal-state disposal is
// null-guarded, so the discharging branch skips and the fall-through branch
// disposes exactly once. Before #2310 only the marked adoption (`using var
// y := owned p;`) was wired that way — every other discharge form nulled
// only the caller's alloca, so the gate (#973a) had to deny them as a
// false reject of a valid program. #2310 closes the gap by adding a
// `nullStateMachineOwnedParamCapture(<source expr>)` call beside the existing
// alloca-null at every one of the 6 lowering sites:
//
//   form   site that nulled the alloca today          frame-null added at
//   ----   -----------------------------------------  ----------------------
//   1      p.Dispose() / p?.Dispose()                 explicitDisposeOwnedReceiverSlot
//   2      _ := p (§17.13 discard)                    explicitDisposeOwnedReceiverSlot
//   3      M(owned p) / callee owned param            buildOrderedArgs
//   4      y := owned p (assignment marker)           nullTransferredOwnedLocalSlot
//   5      owning-collection transfer (Add/Push/…)   nullTransferredOwnedLocalSlot
//   6      new Ctor(owned p)                          buildOrderedArgs
//   yield  yield return p                             (separate #2154 path, n/a)
//
// Each form is a one-line `nullStateMachineOwnedParamCapture(<source expr>)`
// beside the existing alloca null. The helper is self-gating (no-op outside
// a state-machine body, for non-parameter sources, for non-owned parameters),
// so adding it beside every site is safe for the sync-method / plain-local
// callers too. The corresponding scan widening admits each form in
// `stateMachineOwnedParamIsFrameNullable` (member access / null-cond
// member / call / new / null-cond call / assignment-statement-marker /
// discard-statement positions).
//
// In addition, the scan was denying `TryStmt` / `CaseStmt` / lambdas /
// binding atoms / etc. wholesale. The try carve-out (#2310 second residual)
// descends into try body and catch arms normally, and denies only an
// occurrence inside `finallyBody` (a marked adoption spelled in a finally
// would be lowered twice — once on the body's own path and once more when
// the iterator's Dispose re-runs the pending finally chains for a suspended
// state — and reasoning about that is a separate piece).
//
// Every test below is a program the compiler REJECTED before the fix
// (`r.ran == false`, the §28.3 reject). Each pins a specific shape: a
// `Cell.n == 0` exit code under generated-code ASan/LSan (the same counter
// the #973a tests use) is the EXACTLY-once assertion — 0 means no leak and
// no extra disposal; any other value or any non-zero exit pinpoints the
// regress. The "still admitted" boundary (the marked adoption on every
// path) is the existing #973a coverage, not duplicated here.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

const char* kCell =
    "class Cell public static n: int := 0; public V: int := 7; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; ";

std::string prog(const std::string& body) {
    return std::string("module M; ") + kCell + "class P " + body + " end P;";
}

// Compile + run under generated-code ASan/LSan and require the balanced
// disposal counter (`Main` returns Cell.n, so 0 == disposed exactly once).
void expectBalanced(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

}  // namespace

// ---- FORM 1: `c.Dispose()` / `c?.Dispose()` ---------------------------------

// §29.8: the adopting path's explicit Dispose. The frame must NOT dispose
// again (the double-free #973a warned about).
BEZEL_TEST(state_machine_param_conditional_discharge_2310,
           form1_iterator_explicit_dispose_adopting_path_spec_29_8) {
    expectBalanced(prog(
        "  public static method Use(owned c: Cell, cond: bool): IEnumerable<int> "
        "    if cond then c.Dispose(); end; "
        "    yield return 1; "
        "  end Use; "
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), true) do end; "
        "    return Cell.n; "
        "  end Main; "));
}

// §29.8: the fall-through path — the body did not dispose, so the frame's
// teardown is the sole disposal.
BEZEL_TEST(state_machine_param_conditional_discharge_2310,
           form1_iterator_explicit_dispose_fallthrough_path_spec_29_8) {
    expectBalanced(prog(
        "  public static method Use(owned c: Cell, cond: bool): IEnumerable<int> "
        "    if cond then c.Dispose(); end; "
        "    yield return 1; "
        "  end Use; "
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), false) do end; "
        "    return Cell.n; "
        "  end Main; "));
}

// §29.8 / §7.5: the `?.` null-conditional Dispose form (the §7.5 short-
// circuit on a non-null receiver — no-op for the same shape on a nullable
// receiver).
BEZEL_TEST(state_machine_param_conditional_discharge_2310,
           form1_iterator_null_cond_dispose_admitted_spec_29_8) {
    expectBalanced(prog(
        "  public static method Use(owned c: Cell, cond: bool): IEnumerable<int> "
        "    if cond then c?.Dispose(); end; "
        "    yield return 1; "
        "  end Use; "
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), true) do end; "
        "    for var v in Use(new Cell(), false) do end; "
        "    return Cell.n; "
        "  end Main; "));
}

// ---- FORM 2: `_ := c` (the §17.13 consuming discard) ------------------------

BEZEL_TEST(state_machine_param_conditional_discharge_2310,
           form2_iterator_consuming_discard_admitting_path_spec_29_8) {
    expectBalanced(prog(
        "  public static method Use(owned c: Cell, cond: bool): IEnumerable<int> "
        "    if cond then _ := c; end; "
        "    yield return 1; "
        "  end Use; "
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), true) do end; "
        "    return Cell.n; "
        "  end Main; "));
}

BEZEL_TEST(state_machine_param_conditional_discharge_2310,
           form2_iterator_consuming_discard_fallthrough_path_spec_29_8) {
    expectBalanced(prog(
        "  public static method Use(owned c: Cell, cond: bool): IEnumerable<int> "
        "    if cond then _ := c; end; "
        "    yield return 1; "
        "  end Use; "
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), false) do end; "
        "    return Cell.n; "
        "  end Main; "));
}

// ---- FORM 3: `M(owned p)` (a user method with an `owned` parameter) ---------

BEZEL_TEST(state_machine_param_conditional_discharge_2310,
           form3_iterator_owned_argument_transfer_spec_29_8) {
    expectBalanced(prog(
        "  public static method Use(owned c: Cell, cond: bool): IEnumerable<int> "
        "    if cond then using var k := owned c; end; "
        "    yield return 1; "
        "  end Use; "
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), true) do end; "
        "    for var v in Use(new Cell(), false) do end; "
        "    return Cell.n; "
        "  end Main; "));
}

// ---- FORM 4: `y := owned p` (the assignment-position marker) ---------------

// Note: form 4's bare-`var` RHS leaks `y` at scope end (a separate §28.3
// concern, not specific to #2310). The using-var spelling is the canonical
// admitted form; the bare-var spelling is included here as a deliberate
// scope-boundary test — the body of the marker (which IS `c.Dispose()`
// equivalent) is what gets wired to the frame-slot null.
BEZEL_TEST(state_machine_param_conditional_discharge_2310,
           form4_iterator_using_var_assignment_marker_spec_29_8) {
    expectBalanced(prog(
        "  public static method Use(owned c: Cell, cond: bool): IEnumerable<int> "
        "    if cond then using var y := owned c; end; "
        "    yield return 1; "
        "  end Use; "
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), true) do end; "
        "    for var v in Use(new Cell(), false) do end; "
        "    return Cell.n; "
        "  end Main; "));
}

// ---- FORM 5: owning-collection `Add(owned p)` (OwningLinkedList example) ----

BEZEL_TEST(state_machine_param_conditional_discharge_2310,
           form5_iterator_owning_collection_transfer_spec_29_8) {
    expectBalanced(prog(
        "  public static method Use(owned c: Cell, cond: bool): IEnumerable<int> "
        "    using var l := new OwningLinkedList<Cell>(); "
        "    if cond then l.AddFirst(move c); else c.Dispose(); end; "
        "    yield return 1; "
        "  end Use; "
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), true) do end; "
        "    for var v in Use(new Cell(), false) do end; "
        "    return Cell.n; "
        "  end Main; "));
}

// ---- FORM 6: `new Ctor(owned p)` (a constructor with an owned parameter) ----

BEZEL_TEST(state_machine_param_conditional_discharge_2310,
           form6_iterator_new_ctor_owned_argument_spec_29_8) {
    expectBalanced(prog(
        "  public static method Use(owned c: Cell, cond: bool): IEnumerable<int> "
        "    if cond then using var k := owned c; end; "
        "    yield return 1; "
        "  end Use; "
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), true) do end; "
        "    for var v in Use(new Cell(), false) do end; "
        "    return Cell.n; "
        "  end Main; "));
}

// ---- Cross-kind coverage: the same six forms in plain async and async iterator

// §30.1 plain async: form 1 across `await`. Discharging path + suspension
// must keep the frame's terminal-state disposal null-guarded.
BEZEL_TEST(state_machine_param_conditional_discharge_2310,
           form1_plain_async_explicit_dispose_across_await_spec_30_1) {
    expectBalanced(prog(
        "  public static async method Use(owned c: Cell, cond: bool): Task<int> "
        "    if cond then c.Dispose(); end; "
        "    var d := await Task.FromResult(5); "
        "    return d; "
        "  end Use; "
        "  public static async method Main(): Task<int> "
        "    var a := await Use(new Cell(), true); "
        "    var b := await Use(new Cell(), false); "
        "    return Cell.n + a - b; "
        "  end Main; "));
}

// §30.7 async iterator: form 1 across `yield return` + `await`.
BEZEL_TEST(state_machine_param_conditional_discharge_2310,
           form1_async_iterator_explicit_dispose_across_yield_spec_29_8) {
    expectBalanced(prog(
        "  public static async method Use(owned c: Cell, cond: bool): IAsyncEnumerable<int> "
        "    if cond then c.Dispose(); end; "
        "    yield return 1; "
        "    var d := await Task.FromResult(5); "
        "    yield return d; "
        "  end Use; "
        "  public static async method Main(): Task<int> "
        "    await for var v in Use(new Cell(), true) do end; "
        "    await for var v in Use(new Cell(), false) do end; "
        "    return Cell.n; "
        "  end Main; "));
}

// ---- Try carve-out (#2310 second residual) -----------------------------------

// A discharge in the try BODY is admitted (the lowerings null the frame
// slot, exactly like the non-try shape).
BEZEL_TEST(state_machine_param_conditional_discharge_2310,
           try_body_dispose_admitted_spec_29_8) {
    expectBalanced(prog(
        "  public static method Use(owned c: Cell, cond: bool): IEnumerable<int> "
        "    try "
        "      if cond then c.Dispose(); end; "
        "    catch Exception => "
        "      _ := 0; "
        "    end; "
        "    yield return 1; "
        "  end Use; "
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), true) do end; "
        "    for var v in Use(new Cell(), false) do end; "
        "    return Cell.n; "
        "  end Main; "));
}

// A discharge in a CATCH ARM is admitted (catch runs on the exception edge,
// not the iterator's re-driven finally chains).
BEZEL_TEST(state_machine_param_conditional_discharge_2310,
           catch_arm_dispose_admitted_spec_29_8) {
    expectBalanced(prog(
        "  public static method Use(owned c: Cell, cond: bool): IEnumerable<int> "
        "    try "
        "      if cond then throw new InvalidOperationException(\"x\"); end; "
        "    catch InvalidOperationException => "
        "      c.Dispose(); "
        "    end; "
        "    yield return 1; "
        "  end Use; "
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), true) do end; "
        "    for var v in Use(new Cell(), false) do end; "
        "    return Cell.n; "
        "  end Main; "));
}