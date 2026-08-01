// Spec-derived regression tests for #2512 — producerState name-head Owned
// fallback (Ownership.cpp:~3726) carries a stale 12-name literal missing the
// three owning iterable-interface heads (IOwningEnumerator,
// IOwningAsyncEnumerable, IOwningAsyncEnumerator) that #2369 added to
// `kOwningTypeHeads`. The fallback's job is to classify fresh-frame builtin
// returns (e.g. `Shared<T>.Clone()`), but it is also reached for user-defined
// async yield-iterators returning one of the three owning iterable-interface
// heads via the `ce->callee → MethodDecl` chain that resolves the callee but
// skips every inner early-return (returnOwned=false, OwningCollection=false,
// Task-family=false, static-factory=false) — the call lands here, the literal
// is missing the head, and the result is misclassified Borrowed. A user
// `var x := SomeAsyncGen();` (no Discharge/move/Dispose) then carries no
// disposal obligation: no §28.3 auto-drop flag, no scope-end dispose → the
// fresh async state machine leaks at program exit.
//
// §28.3 (v0.221, #1787 ruling; #2369): "The owning heads (IOwningEnumerable<T>,
// IOwningEnumerator<T>, IOwningAsyncEnumerable<T>, IOwningAsyncEnumerator<T>)
// have NO unmarked instance or extension spelling: ... so an unmarked instance
// or extension return typed at an owning head is rejected at the declaration".
// After #2369 a fresh producer that returns one of these heads is one of:
//   1. A builtin (no MethodDecl) — the fallback's documented consumer.
//   2. A user async yield-iterator (`public async method Gen(): IOwning*<T>`
//      with `yield return …`) — the implicit iterator-convention declaration
//      where the head is the announcement, no `owned` marker needed.
//
// The test exercises (2) — the only reachable user-defined fresh-producer path
// for the three sibling heads after #2369's def-site rejection of unmarked
// instance/extension returns. The runtime dispose of the state machine frame
// is verified through the §28.3 auto-drop must-use warning the ownership pass
// emits on an undischarged owned local at scope end; without the fix the
// warning does NOT fire (Borrowed → no obligation → no warning), with the fix
// it does (Owned → obligation registered → warning + scope-end dispose). The
// pre-fix `Borrowed` verdict is the leak: the state machine is heap-allocated
// by the async-method lowering and never reclaimed.
//
// Fix: replace the 12-name literal at Ownership.cpp:~3726 with
// `common::isOwningTypeHead(head)` so the fallback consults the same
// `kOwningTypeHeads` set the rest of the ownership pass uses — the #1414
// registry refactor applied retroactively.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;
using bezel::test::hasAutoDropWarning;

namespace {

std::string mod(const std::string& body) {
    return std::string(
        "module M; "
        "class Box "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Box; ") +
        body;
}

} // namespace

// ===========================================================================
// THE ISSUE REPROS — pre-fix these silently classify the fresh frame Borrowed,
// so the §28.3 auto-drop must-use warning does NOT fire; post-fix it does.
// ===========================================================================

// §28.3 / §28.6 (v0.221, #1787 ruling; #2369) / #2512: a static async
// yield-iterator returning IOwningAsyncEnumerable<Box>. `var x := Gen();` is
// the issue's core repro: pre-fix `x` is Borrowed (no obligation, no
// auto-drop warning); post-fix `x` is Owned (must-use warning fires).
BEZEL_TEST(producer_state_iowning_async_enumerable_2512,
           iowning_async_enumerable_static_async_yield_unclassified) {
    auto r = parseAndCheckOwnership(mod(
        "class App "
        "  public static async method Gen(): IOwningAsyncEnumerable<Box> "
        "    yield return new Box(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    do "
        "      var x := Gen(); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end App;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "x"));
}

// §28.3 / §28.6 (v0.221, #1787 ruling; #2369) / #2512: instance-method
// variant — the receiver-bound async yield-iterator. Same defect shape
// (inner `if (md = calleeMethodDecl)` block falls through every early-return,
// name-head fallback consults the stale literal, head not present → Borrowed).
BEZEL_TEST(producer_state_iowning_async_enumerable_2512,
           iowning_async_enumerable_instance_async_yield_unclassified) {
    auto r = parseAndCheckOwnership(mod(
        "class App "
        "  public async method Gen(): IOwningAsyncEnumerable<Box> "
        "    yield return new Box(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var app: App := new App(); "
        "    do "
        "      var x := app.Gen(); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end App;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "x"));
}

// ===========================================================================
// ACCEPT CONTROLS — shapes the fix must NOT regress.
// ===========================================================================

// The IOwningEnumerable sync-yield sibling (the fallback already classifies
// this Owned pre-#2512 because IOwningEnumerable IS in the literal) — pin
// the working surface so the fix's ACCEPT controls prove the bug is
// narrow, not a regression of the four-head taxonomy.
BEZEL_TEST(producer_state_iowning_async_enumerable_2512,
           iowning_enumerable_sync_yield_auto_dropped_control) {
    auto r = parseAndCheckOwnership(mod(
        "class App "
        "  public method Gen(): IOwningEnumerable<Box> "
        "    yield return new Box(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var app: App := new App(); "
        "    do "
        "      var x := app.Gen(); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end App;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "x"));
}

// `owned IOwningAsyncEnumerable<T>` return — handled at the inner
// `md->returnOwned` early-return (line 3480), never reaches the name-head
// fallback. Pin that the explicit `owned` marker still classifies Owned
// post-fix (a regression there would silently mis-dispose the user's
// transfer path).
BEZEL_TEST(producer_state_iowning_async_enumerable_2512,
           owned_iowning_async_enumerable_return_unaffected) {
    auto r = parseAndCheckOwnership(mod(
        "class App "
        "  public static async method Gen(): "
        "      owned IOwningAsyncEnumerable<Box> "
        "    yield return new Box(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    do "
        "      var x := Gen(); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end App;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "x"));
}