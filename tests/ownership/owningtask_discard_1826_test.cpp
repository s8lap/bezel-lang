// Ownership — issue #1826 (§30.5 vs §17.13, v0.196). The explicit discard
// `_ := await <OwningTask<T>>;` (Pattern B) is an owned-consuming position
// (the discard sinks and disposes the awaited result at end-of-statement),
// exactly like `using var x := await ...`.
//
// §17.13: the explicit discard `_ := expression;` of an OWNED-producing
// value is an *unbound owned temporary* — §28.3 reclaims it at the end of
// the statement, regardless of whether the operand is a fresh `new`/owned-
// returning call (`_ := new Cell();`, `_ := OpenLog();`) or a Pattern B
// `await` (`_ := await MakeOwningTask()`).
//
// §30.5 Pattern-B site check: `consumeAwait*` is the set of awaits that
// occupy an owned-consuming position; the Pattern-B reject site check at
// `MethodOwnership::checkAwaitSitesExpr` only fires when the await is NOT
// in the set. The pre-#1826 walker `collectOwnedConsumingAwaitsStmt` had
// no `DiscardStmt` case, so `_ := await t;` fell off the set and the
// Pattern-B check rejected the program — see
// async_owningtask_discard_1826__discard_accepted_30_5_17_13.bzl for the
// compile-time acceptance + the runtime gtest in
// async_owningtask_discard_1826_test.cpp (the AOT runAot variant) for the
// end-of-statement disposal.
//
// Pre-#1826: bezelc rejects `_ := await t;` with "awaiting an `OwningTask<T>`
// (Pattern B) must appear in an owned-consuming context — a `using var`/`var`
// binding, an `owned` argument, an owned-field/owned-slot assignment, or an
// owned `return`". Post-#1826 the diagnostic mentions the explicit `_ :=
// ...` discard (§17.13) and the program compiles; the discard then disposes
// the awaited cell at end-of-statement. The companion reject test
// (`..._bare_await_rejected_30_5.bzl`) pins that the post-#1826 message
// still trips for the bare-statement `await t;` form (no consumer at all).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

bool ownershipHasError(const bezel::test::OwnershipResult& r,
                       const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// REGRESSION CHECK: pre-#1826 this rejected with "must appear in an
// owned-consuming context" (the Message text of the Pattern-B reject site
// check). Post-#1826 the OWNERSHIP pass accepts: the discard is now an
// owned-consuming position and the await does not trip the Pattern-B
// reject. // per §30.5 / §17.13
BEZEL_TEST(owningtask_discard_1826, discard_of_owningtask_await_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell "
        "  public constructor() end constructor; "
        "end Cell; "
        "class C "
        "  public static async method MakeAsync(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end MakeAsync; "
        "  public static async method Main(): Task<int> "
        "    using var t := C.MakeAsync(); "
        "    _ := await t; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(
        ownershipHasError(r, "must appear in an owned-consuming context"));
}

// CONTROL: the canonical form `using var g := await t;` already compiled
// pre-#1826. Pin that the post-#1826 fix did not regress it. // per §30.5
BEZEL_TEST(owningtask_discard_1826, using_var_owns_await_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell "
        "  public constructor() end constructor; "
        "end Cell; "
        "class C "
        "  public static async method MakeAsync(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end MakeAsync; "
        "  public static async method Main(): Task<int> "
        "    using var t := C.MakeAsync(); "
        "    using var g := await t; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(
        ownershipHasError(r, "must appear in an owned-consuming context"));
}

// CONTROL — the bare `await t;` (no consumer at all) is STILL a reject
// after #1826; the discard fix adds an ADMITTED position, not a relaxation
// of every-non-binding consumption. Per §30.5 the result is dropped
// without disposal — the bare-statement form never had an
// owned-consuming sink. // per §30.5
BEZEL_TEST(owningtask_discard_1826, bare_await_statement_still_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell "
        "  public constructor() end constructor; "
        "end Cell; "
        "class C "
        "  public static async method MakeAsync(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end MakeAsync; "
        "  public static async method Main(): Task<int> "
        "    using var t := C.MakeAsync(); "
        "    await t; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(
        ownershipHasError(r, "must appear in an owned-consuming context"));
}
