// Spec-derived tests for #1660 "Returning a deferred-activation frame"
// (§28.3, v0.184) — the SOUND resolution after the adversarial review found a
// reachable double-free in the owning-head admission.
//
// §28.3 (v0.184): "a return whose declared type is one of the deferred-
// activation kinds — the §29.3/§30.7 iterable and enumerator interfaces, a
// lazy query, or a `Task`-family type" may return a borrow "provided every
// borrow the returned frame carries is sourced from the method's own
// parameters or receiver ... the declared return type itself announces the
// borrow-carrying classification, and every caller re-establishes the
// returned frame's constraints against **its own** receiver and arguments."
//
// The exemption is SOUND for the NON-owning deferred-activation heads —
// `IEnumerable<T>`, `IEnumerator<T>`, `IAsyncEnumerable<T>`, `Task<T>` — which
// are NOT consumer-disposed: a borrow aliased through them is harmless because
// no caller ever disposes the result. These are the spec's own motivating
// shape (`Evens(xs): IEnumerable<int> => xs.Where(IsEven)`) and are admitted
// (they never reach checkBorrowReturnedThroughOwningType — they are not owning
// heads).
//
// It is UNSOUND for the OWNING deferred-activation heads — `OwningTask<T>` and
// `IOwningEnumerable<T>`. These ARE consumer-disposed (producerState's
// name-head fallback classifies a call result of these types Owned) and their
// disposal is NOT idempotent-protected (unlike Shared/Weak/ChannelSender/
// CancellationTokenSource). Admitting a borrow return through them is sound
// only for a DIRECT call, where producerState resolves the concrete all-borrow
// method (methodReturnsOnlyWrapperReceiverBorrow) and downgrades the result to
// Borrowed. Under interface / virtual-override / imported / function-value
// dispatch the callee is bodyless or null (common::calleeMethodDecl), so
// producerState falls to the name-head Owned fallback and the caller disposes
// the borrow — a HARD double-free of the borrow's real owner (confirmed via
// native AOT+ASan for all four dispatch forms; a pre-existing hole in the
// #1493 receiver-owned-field carve-out, independent of #1660). Because the
// caller cannot in general resolve the concrete method, and the name-head
// Owned fallback is load-bearing for fresh iterator/async returns through the
// SAME signature (so it cannot be flipped to Borrowed without leaking those),
// no call-site classification is both sound and non-regressing. The compiler
// therefore REJECTS a borrow returned through `OwningTask<T>` /
// `IOwningEnumerable<T>`; the remedy is the spec's own — return the borrow
// through the non-owning interface (`IEnumerable<T>`/`Task<T>`) or declare
// `owned` for a fresh value.
//
// All shapes below were hand-verified against `build/bezelc -c` (and the
// reject shapes additionally against native AOT+ASan) before being folded in.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

std::string mod(const std::string& body) {
    return std::string(
        "module M; "
        "class Cell "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Cell; ") +
        body;
}

}  // namespace

// ===========================================================================
// REJECT: a borrow returned through an OWNING deferred-activation head.
// ===========================================================================

// A plain (non-`owned`) parameter returned through `OwningTask<T>` — a borrow
// aliased through a consumer-disposed, non-idempotent head. Must be rejected
// (else the caller double-frees the caller's own task under indirect dispatch).
BEZEL_TEST(deferred_activation_frame_return_1660,
           owning_task_param_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(t: OwningTask<Cell>): OwningTask<Cell> => t; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow may not be returned through the owning-type"));
}

// The `IOwningEnumerable<T>` sibling — a plain parameter passthrough.
BEZEL_TEST(deferred_activation_frame_return_1660,
           iowning_enumerable_param_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(src: IOwningEnumerable<Cell>): "
        "      IOwningEnumerable<Cell> => src; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow may not be returned through the owning-type"));
}

// A receiver `owned`-field read returned through `IOwningEnumerable<T>` — the
// pre-existing #1493 carve-out shape. This is the exact source of the native
// double-free (form 1: `return field;` through an interface). Must reject.
BEZEL_TEST(deferred_activation_frame_return_1660,
           iowning_enumerable_receiver_field_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: IOwningEnumerable<Cell>; "
        "  public constructor(owned f: IOwningEnumerable<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): IOwningEnumerable<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow may not be returned through the owning-type"));
}

// The `OwningTask<T>` receiver-owned-field sibling.
BEZEL_TEST(deferred_activation_frame_return_1660,
           owning_task_receiver_field_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: OwningTask<Cell>; "
        "  public constructor(owned f: OwningTask<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningTask<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow may not be returned through the owning-type"));
}

// ★ SOUNDNESS REGRESSION GUARD — the coverage gap that let the double-free
// slip: every prior #1660 test was a DIRECT call, but the hard double-free
// only manifests under INDIRECT dispatch, where the call site cannot resolve
// the concrete borrow-returning method and defaults to Owned. The def-site
// check runs on the concrete override REGARDLESS of how it is called, so the
// fix is that the concrete method's borrow return is rejected at its
// definition — no compiling program can then contain it, and no caller can
// double-free. This is the exact native-AOT+ASan repro (interface dispatch);
// it FAILS on the pre-fix admit (compiles -> SEGV in the caller's disposal)
// and PASSES on the fix (the override's `return field;` is rejected).
BEZEL_TEST(deferred_activation_frame_return_1660,
           iowning_enumerable_borrow_return_via_interface_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "interface ISeqSource "
        "  method Get(): IOwningEnumerable<Cell>; "
        "end ISeqSource; "
        "class Holder : ISeqSource "
        "  public owned field: IOwningEnumerable<Cell>; "
        "  public constructor(owned f: IOwningEnumerable<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): IOwningEnumerable<Cell> return field; end Get; "
        "end Holder; "
        "class P "
        "  public static method Consume(src: ISeqSource): int "
        "    using var t := src.Get(); "
        "    return 0; "
        "  end Consume; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow may not be returned through the owning-type"));
}

// ===========================================================================
// ACCEPT: the spec's motivating requirement — the NON-owning deferred-
// activation heads (not consumer-disposed, so a borrow alias is harmless).
// ===========================================================================

// §28.3's own example: a non-iterator method composing and returning a lazy
// query through the NON-owning `IEnumerable<T>` — every carried borrow is
// parameter-sourced. Admitted (IEnumerable is not an owning head).
BEZEL_TEST(deferred_activation_frame_return_1660,
           nonowning_ienumerable_composed_frame_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Program "
        "  public static method IsEven(x: int): bool => x mod 2 = 0; "
        "  public method Evens(xs: Array<int>): IEnumerable<int> "
        "    => xs.Where(IsEven); "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A plain parameter passed straight back through `IEnumerable<T>` — a borrow
// through a non-consumer-disposed head; admitted.
BEZEL_TEST(deferred_activation_frame_return_1660,
           nonowning_ienumerable_param_passthrough_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class P "
        "  public static method Passthrough(src: IEnumerable<int>): "
        "      IEnumerable<int> => src; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The `Task<T>` sibling: composing/returning a Task-family frame through the
// NON-owning `Task<T>` return type. Admitted.
BEZEL_TEST(deferred_activation_frame_return_1660,
           nonowning_task_composed_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public property Value: int; "
        "  public constructor(v: int) Value := v; end constructor; "
        "end Res; "
        "class P "
        "  public static async method ComputeAsync(r: Res): Task<int> "
        "    return r.Value; "
        "  end ComputeAsync; "
        "  public method Compute(r: Res): Task<int> => ComputeAsync(r); "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// ACCEPT controls: shapes the fix must NOT regress.
// ===========================================================================

// A genuinely FRESH `OwningTask<T>` return (async method minting a new value)
// is not a borrow — it stays admitted (consumer-owned, name-head Owned).
BEZEL_TEST(deferred_activation_frame_return_1660,
           fresh_owning_task_return_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static async method Make(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end Make; "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A receiver `owned`-field read returned through an idempotent-safe owning
// WRAPPER (`Shared<T>`) keeps the #1493 carve-out — its double-dispose is a
// safe refcount no-op (verified: the native interface-dispatch repro runs
// clean), so the fix must NOT tighten it. Scope guard that the reject above is
// limited to the non-idempotent deferred-activation heads.
BEZEL_TEST(deferred_activation_frame_return_1660,
           shared_receiver_field_returned_still_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "class Holder "
        "  public owned field: Shared<Cell>; "
        "  public constructor(owned f: Shared<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): Shared<Cell> return field; end Get; "
        "end Holder;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
