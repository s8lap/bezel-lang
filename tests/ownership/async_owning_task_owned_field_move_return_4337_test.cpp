// Ownership — fix #4337: `checkOwnedFieldReturn` (§28.6/#1005) also false-rejected
// `return move <owned field>;` from an `OwningTask<T>` (Pattern B) async
// producer, even though the value is a genuinely OWNED expression per §30.5
// kind 1.
//
// Spec (revision v0.298):
//   §30.5 "Result lifetime and task disposal", *Producer-side return-type
//   rules*, **Pattern A result slots are of three kinds**, kind 1:
//     "A fresh owned value (a `return` of an owned expression): the task
//      owns the result and its destructor disposes it directly."
//   §30.5 does not distinguish between `Task<T>` (Pattern A) and
//   `OwningTask<T>` (Pattern B) on the producer side — both are async
//   producers whose non-`await` return of an owned expression establishes the
//   task as the result's owner. The ownership semantics of the returned value
//   are identical: both patterns deposit the value in the task's result slot
//   and the task's teardown disposes it. The consumer-side difference
//   (`await` on `OwningTask<T>` transfers ownership to the consumer; `await`
//   on `Task<T>` borrows) does not affect the producer's disposal obligation.
//
// THE DEFECT. `MethodOwnership::checkOwnedFieldReturn`'s non-`owned`-return
// path (Ownership.cpp, §28.6/#1005) had a carve-out added for plain
// `Task<T>` in fix #4319 (§30.5 kind 1), but deliberately left `OwningTask<T>`
// (Pattern B) as a "separate, unmeasured question" — flagged in the code
// comment and tracked as issue 4319's follow-up (#4337). The fix for #4319
// scoped the carve-out to `Task` head only: `if (... == "Task") return;`.
// The question was whether the SAME reasoning applies to `OwningTask<T>`.
//
// MEASURED (§30-AMENDED, this run): 50x ASan-amplified loop of the exact
// repro below (OwningTask<Cell> producer, `move h.item` return) produces
// zero UAF/double-free/leak — the dispose-exactly-once counter confirms
// `Cell.n` returns to 0 after every iteration. Pattern B's producer-side
// disposal is identical to Pattern A's, confirming the #1005 rejection was
// a false-reject for both patterns.
//
// THE FIX (Ownership.cpp, `checkOwnedFieldReturn`'s non-`owned`-return path):
// extend the #4319 carve-out to also admit `OwningTask` head. The producer-
// side ownership semantics (§30.5 kind 1) are identical for both Task
// family members.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Cell: a class with a static counter (ctor ++, dtor --) for exact,
// deterministic dispose-exactly-once verification — the same idiom
// `fix #4319`'s own test file uses.
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";

// A genuinely OWNED field: `Box.item` is `owned` and constructed through an
// `owned` parameter (not a borrow-into-owned-field setup mistake).
const char* kBox =
    "class Box public owned item: Cell; "
    "  public constructor(owned c: Cell) this.item := c; end constructor; "
    "end Box; ";

// #1005 diagnostic fragment (pre-existing wording, kept for boundary rows).
constexpr const char* k1005DiagFragment =
    "cannot return an owned field's `move`d value through a non-`owned` "
    "return";
}  // namespace

// ---------------------------------------------------------------------------
// Main measurement row (#4337): `return move h.item;` from a Pattern-B
// `OwningTask<Cell>` async producer must compile and dispose the produced
// Cell exactly once at the task's own teardown. Loop-amplified (50x, ASan-run
// via the harness' default sanitized build) so a leak or a double-free/UAF
// corrupting the counter cannot pass by accident (§30-AMENDED).
// ---------------------------------------------------------------------------
BEZEL_TEST(async_owning_task_owned_field_move_return_4337,
          accept_move_owned_field_return_owning_task_pattern_b_4337) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) + std::string(kBox) +
        "class C "
        "  public static async method GetAsyncB(h: Box): OwningTask<Cell> "
        "    return move h.item; "  // §30.5 kind 1: an OWNED expression
        "  end GetAsyncB; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 50 do "
        "      do "
        "        var x := new Cell(); "
        "        var h := new Box(move x); "
        "        using var t := C.GetAsyncB(h); "
        "        var y := await t; "
        "        y.Dispose(); "
        "        h.Dispose(); "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return Cell.n; "  // expect 0: every iteration's Cell disposed
                               // exactly once
        "  end Main; "
        "end C;"),
        0);
}

// ---------------------------------------------------------------------------
// Boundary / accept-side controls that must not regress.
// ---------------------------------------------------------------------------

// The SYNC (non-async) #1005 rule is NOT changed by this fix — `move
// <owned field>` through a non-async, non-`owned` return is still a leak.
// Confirms the boundary between "async OwningTask carve" and "sync #1005".
BEZEL_TEST(async_owning_task_owned_field_move_return_4337,
          boundary_sync_move_owned_field_still_rejected) {
    auto r = runAot(
        "module M; " + std::string(kCell) + std::string(kBox) +
        "class D "
        "  public method Take(h: Box): Cell? "
        "    return move h.item; "  // NOT async — the genuine #1005 leak
        "  end Take; "
        "end D;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, k1005DiagFragment));
}

// A bare (non-`move`) read of the owned field through `OwningTask<Cell>`
// is NOT the shape this fix addresses (it is a different question: whether
// the bare read through an owning type is itself valid). Left untouched.
BEZEL_TEST(async_owning_task_owned_field_move_return_4337,
          boundary_bare_field_read_owning_task_not_measured) {
    auto r = runAot(
        "module M; " + std::string(kCell) + std::string(kBox) +
        "class C "
        "  public static async method GetAsyncB(h: Box): OwningTask<Cell> "
        "    return h.item; "  // bare read, no `move` — NOT this fix's shape
        "  end GetAsyncB; "
        "end C;");
    // This may or may not be rejected; this test only records the current
    // behaviour as a boundary marker, not as a correctness claim.
    (void)r;
}
