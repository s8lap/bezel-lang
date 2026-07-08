// Spec-derived tests for the #1493 fix — a bare owned-field borrow-return
// spuriously rejected when the field's type is an owning WRAPPER
// (`ChannelSender<T>`, `Shared<T>`, `Weak<T>`, `OwningTask<T>`,
// `IOwningEnumerable<T>`, `CancellationTokenSource`) rather than a plain user
// class or an owning COLLECTION (§28.6, §28.3, §5.11).
//
// §28.6 bullet 1: "A bare read of an owned field may be returned through a
// non-`owned` return type as a borrow of the field — its source is the
// receiver ... This is leak-free and double-free-free, and applies uniformly
// to a class-typed owned field and to an owning-collection field." §28.3's
// owning-collection/owning-wrapper paragraph (Bezel_Language_Spec.md ~L5268)
// names `Shared<T>`, `Weak<T>`, `OwningTask<T>` explicitly alongside the
// owning collections and restates: "a bare owning-collection field read may
// be returned (its source is the receiver, §28.6)".
//
// Before the fix, `checkBorrowReturnedThroughOwningType`'s owning-WRAPPER
// branch (Ownership.cpp) rejected EVERY borrow unconditionally, including the
// one §28.6 bullet 1 explicitly admits — the receiver's own bare owned-field
// read — while the sibling owning-COLLECTION branch already carried that
// exact carve-out. `borrow_return_owning_type_1007_test.cpp`'s
// `owned_field_read_this_borrow_allowed` pins the collection side; this file
// pins the (previously missing) wrapper side, using `ChannelSender<T>` — the
// issue's repro type, and a stdlib-verified class per Bezel_Standard_Library.md
// §6.9 (`Channel<T>` / `ChannelSender<T>`).
//
// The fix has two parts, both exercised here:
//   1. Definition site (`checkBorrowReturnedThroughOwningType`): admit the
//      receiver-field-borrow shape for wrapper return types too.
//   2. Call site (`producerState`, via the new
//      `methodReturnsOnlyWrapperReceiverBorrow` helper): classify a CALL to
//      such a method as Borrowed, not the wrapper default Owned — otherwise
//      every caller sees a spurious §28.3 auto-drop warning for a value it
//      does not own (memory-safe only via `Dispose()`'s idempotency, §28.2 —
//      still a wrong classification worth pinning).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::parseAndCheckOwnership;

namespace {

// `Holder` owns a `ChannelSender<int>` field. `GetTx` is the issue's exact
// repro shape: a bare owned-field read returned through the matching
// non-`owned` wrapper return type.
std::string mod(const std::string& body) {
    return std::string(
        "module M; "
        "class Holder "
        "  public owned _tx: ChannelSender<int>; "
        "  public constructor(owned tx: ChannelSender<int>) _tx := tx; end constructor; "
        "  destructor end destructor; "
        "  public method GetTx(): ChannelSender<int> => _tx; "
        "end Holder; ") +
        body;
}

} // namespace

// --- ACCEPT: the receiver-field borrow-return §28.6 bullet 1 admits. ---

// #1493 repro (expression body): `_tx` (implicit `this._tx`) returned through
// non-`owned` `ChannelSender<int>` must compile — this was the spurious
// rejection ("a borrow may not be returned through the owning-type return
// `ChannelSender<...>`") the issue reports.
BEZEL_TEST(channel_sender_borrow_return_1493, field_read_this_borrow_allowed) {
    auto r = parseAndCheckOwnership(mod(""));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The same shape via explicit `this._tx` in a block body.
BEZEL_TEST(channel_sender_borrow_return_1493,
           field_read_explicit_this_borrow_allowed) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Holder "
        "  public owned _tx: ChannelSender<int>; "
        "  public constructor(owned tx: ChannelSender<int>) _tx := tx; end constructor; "
        "  destructor end destructor; "
        "  public method GetTx(): ChannelSender<int> "
        "    return this._tx; "
        "  end GetTx; "
        "end Holder;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Sibling wrapper types share the classifier (`isOwningTypeHead` in
// BuiltinRegistry.cpp) that gated the bug — `Shared<T>` gets the same
// dispensation.
BEZEL_TEST(channel_sender_borrow_return_1493, shared_field_read_this_borrow_allowed) {
    auto r = parseAndCheckOwnership(
        "module M; class Cell end Cell; "
        "class Box "
        "  public owned _s: Shared<Cell>; "
        "  public constructor(owned s: Shared<Cell>) _s := s; end constructor; "
        "  destructor end destructor; "
        "  public method GetShared(): Shared<Cell> => _s; "
        "end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- REJECT: every other borrow form (source may not outlive the call). ---

// A borrowed PARAMETER returned through the matching wrapper return type is
// still the #1007 double-free hazard — unaffected by the fix (mirrors
// `shared_param_rejected` in borrow_return_owning_type_1007_test.cpp).
BEZEL_TEST(channel_sender_borrow_return_1493, borrowed_param_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class P "
        "  public static method Identity(s: ChannelSender<int>): ChannelSender<int> "
        "    return s; "
        "  end Identity; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// An owned field of ANOTHER object (not `this`) is still rejected — its
// source need not outlive the call (a scope-bound `other` would dangle).
BEZEL_TEST(channel_sender_borrow_return_1493, field_of_other_object_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Q "
        "  public static method Peek(other: Holder): ChannelSender<int> "
        "    return other._tx; "
        "  end Peek; "
        "end Q;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// --- REJECT: the genuine leak (#1493 must not weaken #1005/#1007). ---

// `move _tx` through the non-`owned` wrapper return is still the #1005 leak —
// the moved value escapes with no owner. This is the check the task asked to
// re-verify: fixing the bare-read dispensation must not admit the transfer
// form too.
BEZEL_TEST(channel_sender_borrow_return_1493, move_field_return_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Holder "
        "  public owned _tx: ChannelSender<int>; "
        "  public constructor(owned tx: ChannelSender<int>) _tx := tx; end constructor; "
        "  destructor end destructor; "
        "  public method LeakTx(): ChannelSender<int> "
        "    return move _tx; "
        "  end LeakTx; "
        "end Holder;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// `move _tx` out of a NON-nullable owned field and returning it leaves the
// field in a moved-from state at method exit with no way to restore it (a
// non-nullable field cannot be nulled) — §28.8.1 rejects this. The #1493 fix
// (which is about bare-read *borrow*-returns) does not, and should not, make
// this clean. (A genuine take-the-field pattern uses a nullable owned field
// `owned _tx: ChannelSender<int>?` with `_tx := null` after the move.)
BEZEL_TEST(channel_sender_borrow_return_1493, move_nonnullable_field_return_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Holder "
        "  public owned _tx: ChannelSender<int>; "
        "  public constructor(owned tx: ChannelSender<int>) _tx := tx; end constructor; "
        "  destructor end destructor; "
        "  public method TakeTx(): owned ChannelSender<int> "
        "    return move _tx; "
        "  end TakeTx; "
        "end Holder;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());  // §28.8.1 moved-from field at exit
}

// A fresh owned producer (a builtin owned-returning call, `Channel<T>.
// NewSender()`) returned through a non-`owned` wrapper return from a STATIC
// method is unaffected by the fix — the pre-existing consumer-owns
// classification (#1007) still applies; must compile clean at the definition
// site (mirrors `shared_clone_owned_producer_clean`).
BEZEL_TEST(channel_sender_borrow_return_1493, fresh_producer_static_clean) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class P "
        "  public static method Wrap(c: Channel<int>): ChannelSender<int> "
        "    return c.NewSender(); "
        "  end Wrap; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Call-site classification (the producerState half of the fix). ---

// Calling `h.GetTx()` (the receiver-borrow shape) and never discharging the
// result must NOT raise the §28.3 auto-drop warning — the callee's result is
// a borrow of `h`'s field, not a value the caller owns. Before the
// producerState fix, this call was (wrongly) tracked Owned, so `b` drew the
// "owned value ... is implicitly disposed at the end of its scope" warning
// even though `h`'s destructor is the field's only real owner.
BEZEL_TEST(channel_sender_borrow_return_1493, caller_no_autodrop_warning) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Use(h: Holder): int "
        "    var b := h.GetTx(); "
        "    return 0; "
        "  end Use; "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "b"));
}

// Soundness guard: a method whose return paths are a MIX of the receiver-
// borrow shape and a genuine fresh producer must NOT be classified Borrowed
// at the call site — only when EVERY return path is the receiver-field-read
// shape is the call result provably a borrow. Here one branch returns `_tx`
// (borrow) and the other mints a fresh sender via `Channel<T>.NewSender()`
// (owned); the caller cannot statically know which branch ran, so it must
// still track the result as Owned and warn on non-discharge — dropping this
// would leak the fresh-producer branch's value.
BEZEL_TEST(channel_sender_borrow_return_1493, mixed_returns_caller_still_owned) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Holder "
        "  public owned _tx: ChannelSender<int>; "
        "  public constructor(owned tx: ChannelSender<int>) _tx := tx; end constructor; "
        "  destructor end destructor; "
        "  public method GetTxMixed(cond: bool, c: Channel<int>): ChannelSender<int> "
        "    if cond then "
        "      return _tx; "
        "    else "
        "      return c.NewSender(); "
        "    end; "
        "  end GetTxMixed; "
        "end Holder; "
        "class P "
        "  public static method Use(h: Holder, cond: bool, c: Channel<int>): int "
        "    var b := h.GetTxMixed(cond, c); "
        "    return 0; "
        "  end Use; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "b"));
}
