// Codegen + ownership — #973a: a CONDITIONALLY-discharged `owned` parameter of a
// state-machine method (§29.8 iterators, §30.1 async / async iterators).
//
// §29.8: "When the iterator is disposed, the state machine disposes any `owned`
// parameter that has not been moved out of by the body." §30.1 states the same
// rule for an async frame ("any `owned` parameter never moved out is disposed by
// the frame when the task reaches its terminal state — on every completion path
// (success, fault, and cancellation)"). "has not been moved out of" is a RUNTIME
// property once the move-out sits on one branch:
//
//     public static method Use(owned c: Cell, cond: bool): IEnumerable<int>
//       if cond then using var x := owned c; end;   -- §28.3 v0.180 marked adoption
//       yield return 1;
//     end Use;
//
// Before this fix both #945 (sync) and #973b (async) required the parameter to be
// undischarged on EVERY path (`everDischarged_ == 0`); the shape above fell to the
// §28.3 "would be dropped here without disposal" reject — sound, but a false
// reject of a valid program (issue #973 part 1, and the pre-existing
// `iterator_owned_param_945.conditional_discharge_still_rejected`).
//
// The fix makes the FRAME capture slot the runtime moved-from flag:
//   - sema (Ownership.cpp) admits the parameter — flagging
//     `iteratorFrameConditionalAutoDispose` — only when
//     common::stateMachineOwnedParamIsFrameNullable proves every occurrence of it
//     is either the marked adoption (whose lowering nulls the frame slot) or a
//     borrow read `c.F` / `c.M(…)`. Any other occurrence keeps the reject.
//   - codegen nulls the FRAME capture slot at the marked adoption
//     (Emitter::nullStateMachineOwnedParamCapture, beside the existing #1978
//     alloca nulling), and the three frame-disposal loops — sync-iterator
//     Dispose, async-iterator emitFreeAndReturn, plain-async completion edges —
//     admit the flag. Each was ALREADY null-guarded, so the guard becomes an
//     exact moved-from test: the adopting path loads null and skips, the
//     fall-through path disposes exactly once.
//   - for an `IEnumerable<T>` iterator the driven frame is the per-loop CLONE, so
//     the #2154 capture claim is armed: the captures move source → clone at
//     GetEnumerator and a second GetEnumerator raises InvalidOperationException
//     (the correct affine reading — a body that may consume the parameter cannot
//     be replayed).
//
// Every test here is a program the compiler REJECTED before the fix, so each
// fails without it (`r.ran == false`, the §28.3 reject captured as a diagnostic)
// and passes with it. `runAotChecked` compiles the generated code with ASan/LSan,
// so a leak, a double-free or a use-after-free flips `r.ran` to false — and the
// static `Cell.n` counter (ctor +1, dtor -1) returned from `Main` independently
// pins EXACTLY-once: 0 means no leak and no extra disposal.

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

// True iff the full pipeline (including the ownership pass) rejects `src`.
bool rejectedAtCompile(const std::string& src) {
    auto r = bezel::test::parseAndCheckOwnership(src);
    return r.diagnostics.hasErrors();
}

// The §29.8 iterator under test: `c` is adopted on ONE branch only.
const char* kCondIter =
    "  public static method Use(owned c: Cell, cond: bool): IEnumerable<int> "
    "    if cond then using var x := owned c; end; "
    "    yield return 1; yield return 2; "
    "  end Use; ";

}  // namespace

// §29.8: the issue's repro — the ADOPTING path. The `using var` disposes the
// parameter; the frame must NOT dispose it again (the double-free #973 warned
// about).
BEZEL_TEST(iterator_owned_param_conditional_973a,
           sync_adopted_path_disposed_once_spec_29_8) {
    expectBalanced(prog(
        std::string(kCondIter) +
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), true) do end; "
        "    return Cell.n; "
        "  end Main; "));
}

// §29.8: the FALL-THROUGH path — the body did not adopt, so the state machine's
// teardown is the sole disposal. Without the frame disposal this is a leak (the
// direction #973 called out as the alternative failure).
BEZEL_TEST(iterator_owned_param_conditional_973a,
           sync_not_adopted_path_disposed_once_spec_29_8) {
    expectBalanced(prog(
        std::string(kCondIter) +
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), false) do end; "
        "    return Cell.n; "
        "  end Main; "));
}

// §29.7 / §29.8: EARLY BREAK on the adopting path — the iterator is abandoned
// while suspended at the first yield, after the adoption has run. The
// abandonment Dispose must skip the already-moved parameter.
BEZEL_TEST(iterator_owned_param_conditional_973a,
           sync_adopted_early_break_disposed_once_spec_29_8) {
    expectBalanced(prog(
        std::string(kCondIter) +
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), true) do break; end; "
        "    return Cell.n; "
        "  end Main; "));
}

// §29.7 / §29.8: early break on the fall-through path — the abandonment Dispose
// is the sole disposal.
BEZEL_TEST(iterator_owned_param_conditional_973a,
           sync_not_adopted_early_break_disposed_once_spec_29_8) {
    expectBalanced(prog(
        std::string(kCondIter) +
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), false) do break; end; "
        "    return Cell.n; "
        "  end Main; "));
}

// §29.8: the adoption sits AFTER the yield, and the consumer breaks at the first
// yield — so the adoption never runs even though `cond` is true. The runtime flag
// (not the static `cond`) is what decides: the frame still owns the parameter and
// must dispose it. A statically-keyed scheme would get this wrong.
BEZEL_TEST(iterator_owned_param_conditional_973a,
           sync_adoption_after_yield_not_reached_disposed_once_spec_29_8) {
    expectBalanced(prog(
        "  public static method Use(owned c: Cell, cond: bool): IEnumerable<int> "
        "    yield return 1; "
        "    if cond then using var x := owned c; end; "
        "  end Use; "
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), true) do break; end; "
        "    return Cell.n; "
        "  end Main; "));
}

// §29.8: the same iterator DRAINED, so the post-yield adoption does run.
BEZEL_TEST(iterator_owned_param_conditional_973a,
           sync_adoption_after_yield_reached_disposed_once_spec_29_8) {
    expectBalanced(prog(
        "  public static method Use(owned c: Cell, cond: bool): IEnumerable<int> "
        "    yield return 1; "
        "    if cond then using var x := owned c; end; "
        "  end Use; "
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), true) do end; "
        "    return Cell.n; "
        "  end Main; "));
}

// §29.8 / §28.4: a MIXED parameter list — `a` conditionally adopted, `b` never
// discharged. Arming the capture claim moves BOTH to the enumerator clone, so
// the source must stop disposing `b` or the two frames would free it twice.
BEZEL_TEST(iterator_owned_param_conditional_973a,
           sync_mixed_conditional_and_never_discharged_spec_29_8) {
    expectBalanced(prog(
        "  public static method Use(owned a: Cell, owned b: Cell, cond: bool): IEnumerable<int> "
        "    if cond then using var x := owned a; end; "
        "    yield return b.V; "
        "  end Use; "
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), new Cell(), true) do end; "
        "    for var v in Use(new Cell(), new Cell(), false) do end; "
        "    return Cell.n; "
        "  end Main; "));
}

// §29.7 / §28.3 exception-path discharge: the consumer throws while the iterator
// is suspended. The unwind reaches the same iterator Dispose, on both branches.
BEZEL_TEST(iterator_owned_param_conditional_973a,
           sync_unwind_from_loop_body_disposed_once_spec_29_7) {
    expectBalanced(prog(
        std::string(kCondIter) +
        "  public static method Drive(cond: bool): int "
        "    try "
        "      for var v in Use(new Cell(), cond) do "
        "        throw new InvalidOperationException(\"x\"); "
        "      end; "
        "    catch InvalidOperationException => "
        "      return Cell.n; "
        "    end; "
        "    return 99; "
        "  end Drive; "
        "  public static method Main(): int "
        "    return Drive(true) + Drive(false); "
        "  end Main; "));
}

// §29.11: the parameter is affine, so only ONE enumerator may claim the frame's
// captures. A second `GetEnumerator` raises InvalidOperationException rather than
// replaying a pointer the first enumeration may already have consumed — and the
// value is still disposed exactly once. (Pre-fix the whole program was rejected;
// the claim path also needed the missing post-`GetEnumerator` pending check,
// without which the intended exception was a SIGSEGV.)
BEZEL_TEST(iterator_owned_param_conditional_973a,
           sync_second_enumeration_raises_and_disposes_once_spec_29_11) {
    expectBalanced(prog(
        std::string(kCondIter) +
        "  public static method Main(): int "
        "    var seen := 0; var caught := 0; "
        "    try "
        "      var src := Use(new Cell(), false); "
        "      for var v in src do seen := seen + v; end; "
        "      for var v in src do seen := seen + v; end; "
        "    catch InvalidOperationException => "
        "      caught := 1; "
        "    end; "
        "    if seen <> 3 then return 71; end; "
        "    if caught <> 1 then return 72; end; "
        "    return Cell.n; "
        "  end Main; "));
}

// §29.8 / §30.7 (#973a, extending #973b): an ASYNC iterator, with an `await`
// BETWEEN the conditional adoption and the end of the body — so the frame is
// suspended across a real suspension point with the moved-from flag already
// written. Both branches, full drain.
BEZEL_TEST(iterator_owned_param_conditional_973a,
           async_iterator_across_await_disposed_once_spec_29_8) {
    expectBalanced(prog(
        "  public static async method Use(owned c: Cell, cond: bool): IAsyncEnumerable<int> "
        "    if cond then using var x := owned c; end; "
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

// §29.7 / §30.7: the async iterator ABANDONED at the first yield (the consumer
// breaks), on both branches.
BEZEL_TEST(iterator_owned_param_conditional_973a,
           async_iterator_abandoned_disposed_once_spec_29_7) {
    expectBalanced(prog(
        "  public static async method Use(owned c: Cell, cond: bool): IAsyncEnumerable<int> "
        "    if cond then using var x := owned c; end; "
        "    yield return 1; "
        "    var d := await Task.FromResult(5); "
        "    yield return d; "
        "  end Use; "
        "  public static async method Main(): Task<int> "
        "    await for var v in Use(new Cell(), true) do break; end; "
        "    await for var v in Use(new Cell(), false) do break; end; "
        "    return Cell.n; "
        "  end Main; "));
}

// §30.1: a PLAIN (non-iterator) `async` method — the frame's terminal-state
// disposal on the success completion edge, with an `await` after the conditional
// adoption. Both branches.
BEZEL_TEST(iterator_owned_param_conditional_973a,
           plain_async_across_await_disposed_once_spec_30_1) {
    expectBalanced(prog(
        "  public static async method Use(owned c: Cell, cond: bool): Task<int> "
        "    if cond then using var x := owned c; end; "
        "    var d := await Task.FromResult(5); "
        "    return d; "
        "  end Use; "
        "  public static async method Main(): Task<int> "
        "    var a := await Use(new Cell(), true); "
        "    var b := await Use(new Cell(), false); "
        "    return Cell.n + a - b; "
        "  end Main; "));
}

// The DELIBERATE BOUND, #2310 (the #973a residual widening): the other discharge
// forms (`c.Dispose()` / `c?.Dispose()` / `_ := c` / `Sink(owned c)` / `new
// Ctor(owned c)` / owning-collection transfer `Add(owned c)`) used to null
// only the caller's alloca, not the frame capture slot, so admitting them would
// have left the frame double-freeing (#973a precisely rejects them as a false
// reject of a valid program, #2310 widens each to admit). The fix wires a
// `nullStateMachineOwnedParamCapture` call beside the existing alloca-null at
// each of the 6 lowering sites and widens the `stateMachineOwnedParamIsFrame-
// Nullable` allowlist correspondingly; each shape now compiles to a
// null-guarded frame-disposal skip on the discharging branch and a normal
// disposal on the fall-through. The Cell counter (`Main` returns `Cell.n`)
// pins EXACTLY-once: 0 means no leak and no extra disposal. Run under
// generated-code ASan/LSan so a leak, double-free, or UAF flips `r.ran` to
// false. // per §29.8 / §30.1, v0.236
BEZEL_TEST(iterator_owned_param_conditional_973a,
           conditional_explicit_dispose_admitted_spec_29_8) {
    expectBalanced(prog(
        std::string(kCondIter).replace(
            std::string(kCondIter).find("using var x := owned c;"), 23,
            "c.Dispose();") +
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), true) do end; "
        "    for var v in Use(new Cell(), false) do end; "
        "    return Cell.n; "
        "  end Main; "));
}

BEZEL_TEST(iterator_owned_param_conditional_973a,
           conditional_discard_admitted_spec_29_8) {
    expectBalanced(prog(
        std::string(kCondIter).replace(
            std::string(kCondIter).find("using var x := owned c;"), 23,
            "_ := c;") +
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), true) do end; "
        "    for var v in Use(new Cell(), false) do end; "
        "    return Cell.n; "
        "  end Main; "));
}

BEZEL_TEST(iterator_owned_param_conditional_973a,
           conditional_owned_argument_admitted_spec_29_8) {
    expectBalanced(prog(
        "  public static method Sink(owned c: Cell): int "
        "    using var k := owned c; return 1; "
        "  end Sink; "
        "  public static method Use(owned c: Cell, cond: bool): IEnumerable<int> "
        "    if cond then var r := Sink(move c); end; "
        "    yield return 1; "
        "  end Use; "
        "  public static method Main(): int "
        "    for var v in Use(new Cell(), true) do end; "
        "    for var v in Use(new Cell(), false) do end; "
        "    return Cell.n; "
        "  end Main; "));
}
