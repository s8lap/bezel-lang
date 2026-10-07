// #4647 — K3.13's return-position redundant-move check was gated only on the
// explicit `owned` keyword (returnOwned_), missing the mint-announcing half
// of K3.2. Per K3.2, a return under an *unmarked* mint-announcing return
// type is also marker-free: `OwningTask<T>` always announces ownership
// transfer at the return position, and `Task<T>` in a non-async body does
// the same.
//
// Spec §28.3 K3.2 (consume table, v0.315): "`return` under an `owned`
// return type, or under an unmarked *mint-announcing* return type —
// `Task`-family in a non-`async` body, iterable-family in a non-iterator
// body ... Spelling = none — the declared return type announces (marker-free)."
// And K3.13's marker economy: the marker is admitted only at the
// marker-required consuming positions; writing it at a marker-free position
// is the redundant-marker error.
//
// Fail-without: with the guard narrowed to returnOwned_ only,
// `return move x;` compiles clean through an OwningTask<T> return in a
// non-async body — the fix fires only when returnOwned_ OR when the
// mint-announcing condition is added to the guard.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// True iff some error diagnostic's message contains `needle`.
bool errorSays(const OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// REJECT: `return move x;` through an OwningTask<T> return in a non-async
// body is the redundant marker — the OwningTask<T> already announces the
// transfer (K3.2 / K3.13).
// ---------------------------------------------------------------------------
BEZEL_TEST(redundant_marker_mint_announcing_return_4647,
           return_move_through_owning_task_rejected_spec_28_3_K3_2) {
    auto r = parseAndCheckOwnership(
        "module M; class Res end Res; class C "
        "  public static method Make(): OwningTask<Res> "
        "    var x := new Res(); "
        "    return move x; "
        "  end Make; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(
        r, "the `move` marker is redundant on a `return` operand"));
}

// ---------------------------------------------------------------------------
// ACCEPT control: `return x;` (unmarked) through an OwningTask<T> return in
// a non-async body compiles clean — the declaration is the announcement.
// ---------------------------------------------------------------------------
BEZEL_TEST(redundant_marker_mint_announcing_return_4647,
           return_unmarked_through_owning_task_accepted_spec_28_3_K3_2) {
    auto r = parseAndCheckOwnership(
        "module M; class Res end Res; class C "
        "  public static method Make(): OwningTask<Res> "
        "    var x := new Res(); "
        "    return x; "
        "  end Make; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// ACCEPT: an `async` method returning OwningTask<T> is EXEMPT from the
// redundant-marker check. In an async body, `return move x;` transfers the
// local into the task frame at the await boundary (§30.1); the async
// completion edge owns the frame, not the caller's bare return position.
// The `!isAsync_` guard is therefore correct: only non-async bodies treat
// the OwningTask<T> return as a marker-free position.
// ---------------------------------------------------------------------------
BEZEL_TEST(redundant_marker_mint_announcing_return_4647,
           async_return_move_through_owning_task_accepted_spec_28_3_K3_2) {
    auto r = parseAndCheckOwnership(
        "module M; class Res end Res; class C "
        "  public static async method Make(): OwningTask<Res> "
        "    var x := new Res(); "
        "    return move x; "
        "  end Make; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// REJECT control: `return move x;` through a plain `owned` return (explicit
// keyword) is also the redundant marker — already covered by returnOwned_,
// included here as the spec K3.2 control row.
// ---------------------------------------------------------------------------
BEZEL_TEST(redundant_marker_mint_announcing_return_4647,
           return_move_through_owned_return_rejected_spec_28_3_K3_2) {
    auto r = parseAndCheckOwnership(
        "module M; class Res end Res; class C "
        "  public static method Make(): owned Res "
        "    var x := new Res(); "
        "    return move x; "
        "  end Make; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(
        r, "the `move` marker is redundant on a `return` operand"));
}

}  // namespace
