// fix #892 / §28.3: mark out-owned Try* recipients owned in if/while conditions.
//
// A Try*(out owned item) call used as the CONDITION of an `if` or `while`
// statement did not mark the receiving local Owned, so dropping `item` was
// wrongly accepted — a silent leak.  The fix (§28.3, #892) processes the
// condition expression through handleDischarge so markOutOwnedArgs fires for
// the condition call, exactly as it does for a statement-level call.
//
// Key guardrails:
//   - the DRAIN LOOP `while s.TryPop(out owned item) do item?.Dispose(); end`
//     must ACCEPT (item is disposed in the body each iteration; the false exit
//     carries the pre-condition state — item was not written — so no discharge
//     obligation escapes the loop);
//   - dropping item in the while body → REJECT;
//   - the borrow-form `if s.TryPop(out item) then end` (non-owning Stack<int>)
//     → ACCEPT (no obligation);
//   - the existing statement-level #860 forms regress unchanged.
//
// // per §28.3, §28.11, #892

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;
using bezel::test::hasAutoDropWarning;

namespace {

// Disposable Cell class + arbitrary Main body.
std::string withMain(const std::string& body) {
    return std::string(
        "module M; "
        "class Cell "
        "  public static _n: int := 0; "
        "  public constructor() Cell._n := Cell._n + 1; end constructor; "
        "  destructor Cell._n := Cell._n - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Main(): int "
        "    do ") +
        body +
        "    end; "
        "    return Cell._n; "
        "  end Main; "
        "end C;";
}

bool hasDropError(const bezel::test::OwnershipResult& r,
                  const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("owned value in `" + name + "`") !=
                std::string::npos &&
            d.message.find("without disposal") != std::string::npos)
            return true;
    return false;
}

} // namespace

// §28.3 / #892 / #1387: `if s.TryPop(out owned var item) then end;` — item
// received ownership in the condition and is never discharged. Under #1387 the
// affine auto-drop rule (§28.3 v0.150) now applies to `out owned var` inline
// locals that are never discharged: emit a MUST-USE WARNING (not an error) and
// register scope-end disposal. Before #1387 this was a hard error.
BEZEL_TEST(out_owned_condition_892, if_cond_drop_rejected) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new OwnedStack<Cell>(); "
        "s.Push(move (new Cell())); "
        "if s.TryPop(out owned var item) then end; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "item"));
}

// §28.3 / #892: `item?.Dispose()` AFTER the if discharges item on both the
// success path (non-null) and the false path (null → Dispose no-ops). ACCEPT.
BEZEL_TEST(out_owned_condition_892, if_cond_dispose_after_accepted) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new OwnedStack<Cell>(); "
        "s.Push(move (new Cell())); "
        "if s.TryPop(out owned var item) then end; "
        "item?.Dispose(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 / #892 (WHILE DRAIN ACCEPT): the idiomatic drain loop
// `while s.TryPop(out owned var item) do item?.Dispose(); end`
// disposes item in the body each iteration and carries no obligation past the
// loop (false exit → TryPop returned false, item not written). Must ACCEPT.
BEZEL_TEST(out_owned_condition_892, while_drain_dispose_body_accepted) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new OwnedStack<Cell>(); "
        "s.Push(move (new Cell())); "
        "s.Push(move (new Cell())); "
        "while s.TryPop(out owned var item) do "
        "  item?.Dispose(); "
        "end; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 / #892 (WHILE DRAIN REJECT): item dropped without disposal in the
// while body → REJECT (obligation incurred by the condition, not discharged).
BEZEL_TEST(out_owned_condition_892, while_drain_drop_rejected) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new OwnedStack<Cell>(); "
        "s.Push(move (new Cell())); "
        "while s.TryPop(out owned var item) do "
        "  // item dropped without disposal "
        "end; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDropError(r, "item"));
}

// §28.3 / #892 (BORROW-FORM GUARDRAIL): `Stack<int>.TryPop(out item)` —
// non-owning collection, value-typed element, no `owned` modifier — carries no
// discharge obligation. ACCEPT. Guards over-broad condition processing.
BEZEL_TEST(out_owned_condition_892, if_borrow_form_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    do "
        "      using var s := new Stack<int>(); "
        "      s.Push(42); "
        "      var item: int := 0; "
        "      if s.TryPop(out item) then end; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 / §28.11 / #860 / #1387 (REGRESSION — statement form): the statement-
// level `s.TryPop(out owned var item)` with an undischarged item now follows
// the same affine auto-drop rule as the if-condition form (#1387): it emits a
// must-use WARNING and registers scope-end disposal, not a hard error. The
// item is still correctly marked Owned (the #860 mark-owned fix is intact).
BEZEL_TEST(out_owned_condition_892, stmt_form_860_regression_rejected) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new OwnedStack<Cell>(); "
        "s.Push(move (new Cell())); "
        "s.TryPop(out owned var item); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "item"));
}

// §28.3 / §28.11 / #860 (REGRESSION — statement form dispose): disposing after
// a statement-level TryPop still ACCEPTS. Guards the #860 positive case.
BEZEL_TEST(out_owned_condition_892, stmt_form_860_regression_dispose_accepted) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new OwnedStack<Cell>(); "
        "s.Push(move (new Cell())); "
        "s.TryPop(out owned var item); "
        "item?.Dispose(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
