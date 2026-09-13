// Spec-derived regression tests for issue #4032 — the two `owned`-return
// double-dispose checks share a single predicate for the §28.3
// "non-disposable-iterable-return" exemption
// (`common::isNonOwningIterFormatEnumerableHead`, BuiltinRegistry.hpp /
// #4004), and that predicate MUST answer {IEnumerable, IAsyncEnumerable} —
// neither wider (which would unsoundly exempt the four owning heads whose
// IDisposable-extension measurably double-frees) nor narrower (which would
// regress #3993's IEnumerable/IAsyncEnumerable exemption on either check).
//
// The bug the issue names: #3993's `checkOwnedReturnDirectBorrow` originally
// spelled the exemption as a hand-rolled `head == "IEnumerable" || head ==
// "IAsyncEnumerable"` site, and #4004/#4005's `checkOneLambdaOwnedReturn
// DirectBorrow` carried a SECOND hand-rolled copy of the SAME two names —
// check_enumerable_grid_spellings (umbrella #3419, the enumerable-wrap
// grid's class-H ratchet) flagged the second copy, because Ownership.cpp
// had not previously carried any class-H spelling and the grid may not
// grow a new site. The fix consolidated the two sites onto the shared
// `isNonOwningIterFormatEnumerableHead` predicate
// (`isIterFormatEnumerableHead(head) &&
//  !isOwningIterableInterfaceHead(head)`, BuiltinRegistry.cpp / #4004),
// reducing both grid sites to ZERO and pinning the predicate's reach.
//
// What this file pins: the predicate's REACH. Six assertion rows:
//   * two ACCEPT rows for {IEnumerable, IAsyncEnumerable} — the predicate
//     fires, no diagnostic;
//   * four REJECT rows for {IEnumerator, IAsyncEnumerator, IOwningEnumerable,
//     IOwningAsyncEnumerable} — the predicate does NOT fire, the
//     double-dispose check fires its own diagnostic (the four HEAD axis
//     points a future widening would unsoundly exempt).
//
// Two pairs are tested PER check — `checkOwnedReturnDirectBorrow` (the
// #3993 method-level check, in `Ownership_Checks.cpp`) and
// `checkOneLambdaOwnedReturnDirectBorrow` (the #4005 lambda-level check,
// in `Ownership.cpp`) — so neither site can silently DRIFT back to a
// hand-spelling of one of the names without breaking this file. The
// remaining two owning-head siblings (IOwningEnumerator,
// IOwningAsyncEnumerator) are exempt from the named heads by the §28.3
// owning-iterator family already (the four-IOwning* reduction, #2369);
// the predicate does not need to enumerate them separately because
// `isIterFormatEnumerableHead` does not contain them — they reach the
// check and fail anyway, and adding them here would only test a name set
// the predicate does NOT consult.
//
// Every row drives `parseAndCheckOwnership` (the in-process ownership-pass
// oracle — tests/support/ownership_support.hpp). The AOT pass is NOT
// used: a successful parse-and-check tells us the predicate either
// returned (no diagnostic) or fell through to the borrow-detection
// diagnostic, exactly what these rows need to verify.
//
// per stdlib §4.12, §4.13, #3419, #3993, #4004, #4005, #4007, #4010,
// #4032

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// The exact predicate's exempt-set message — a borrow-returned-as-owned
// diagnostic must NOT contain either fragment when the predicate fires;
// it must contain ONE of them when the predicate does not fire.
constexpr const char* kOwnedReturnDirectBorrow =
    "a borrow cannot be returned as an `owned` value";

// A module prefix that declares a no-op Cell (destructor fires once per
// fresh `new`; no method bodies, no fields). Used by both the method-level
// (#3993) and the lambda-level (#4005) checks below.
const char* kMod =
    "module M; "
    "class Cell "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "end Cell; ";

}  // namespace

// ===========================================================================
// ACCEPT — `checkOwnedReturnDirectBorrow` (Ownership_Checks.cpp / #3993)
// exempts the {IEnumerable, IAsyncEnumerable} head set via the shared
// `common::isNonOwningIterFormatEnumerableHead` predicate. A borrowed
// parameter passed straight through to an `owned` return at either head
// compiles clean — these are the EXACT two sites that predicate must
// answer true for. If a future change narrows the predicate to one of the
// two names, one of these rows fails. // per stdlib §4.12, #3419, #4004
// ===========================================================================

BEZEL_TEST(shared_non_owning_iter_format_enumerable_head_predicate_4032,
           method_level_ienumerable_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(std::string(kMod) +
        "class P "
        "  public static method Id(a: IEnumerable<Cell>): "
        "      owned IEnumerable<Cell> => a; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(shared_non_owning_iter_format_enumerable_head_predicate_4032,
           method_level_iasync_enumerable_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(std::string(kMod) +
        "class P "
        "  public static method Id(a: IAsyncEnumerable<Cell>): "
        "      owned IAsyncEnumerable<Cell> => a; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// REJECT — `checkOwnedReturnDirectBorrow` (Ownership_Checks.cpp / #3993)
// does NOT exempt any other iterator/enumerator head. The predicate covers
// exactly the {IEnumerable, IAsyncEnumerable} pair; the four-`IOwning*`
// heads UNSOUNDLY exempt nothing here (they extend IDisposable /
// IAsyncDisposable, stdlib §4.13, and measurably double-free through this
// shape). These rows PIN that a future widening of the predicate to
// `isIterFormatEnumerableHead` (which adds the two owning enumerable
// heads) is unsound, and a future NARROWING to a single name regresses
// the sibling ACCEPT row above. // per stdlib §4.13, #3419, #4004, #4007
// ===========================================================================

// The two ENUMERATOR heads — each extends IDisposable / IAsyncDisposable,
// so a borrow-returned-as-owned through either measurably double-frees.
BEZEL_TEST(shared_non_owning_iter_format_enumerable_head_predicate_4032,
           method_level_ienumerator_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(std::string(kMod) +
        "class P "
        "  public static method Id(a: IEnumerator<Cell>): "
        "      owned IEnumerator<Cell> => a; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kOwnedReturnDirectBorrow));
}

BEZEL_TEST(shared_non_owning_iter_format_enumerable_head_predicate_4032,
           method_level_iasync_enumerator_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(std::string(kMod) +
        "class P "
        "  public static method Id(a: IAsyncEnumerator<Cell>): "
        "      owned IAsyncEnumerator<Cell> => a; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kOwnedReturnDirectBorrow));
}

// The two owning ENUMERABLE heads — the head axis a #3993-internal debate
// (#4007 / #4004's "swap to isIterFormatEnumerableHead alone") would have
// UNSOUNDLY widened to. The predicate's `&& !isOwningIterableInterface
// Head(head)` is what rejects these; without the negation these rows
// would (wrongly) compile clean and double-free under ASan.
BEZEL_TEST(shared_non_owning_iter_format_enumerable_head_predicate_4032,
           method_level_iowning_enumerable_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(std::string(kMod) +
        "class P "
        "  public static method Id(a: IOwningEnumerable<Cell>): "
        "      owned IOwningEnumerable<Cell> => a; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kOwnedReturnDirectBorrow));
}

BEZEL_TEST(shared_non_owning_iter_format_enumerable_head_predicate_4032,
           method_level_iowning_async_enumerable_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(std::string(kMod) +
        "class P "
        "  public static method Id(a: IOwningAsyncEnumerable<Cell>): "
        "      owned IOwningAsyncEnumerable<Cell> => a; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kOwnedReturnDirectBorrow));
}

// ===========================================================================
// ACCEPT — `checkOneLambdaOwnedReturnDirectBorrow` (Ownership.cpp / #4005)
// exempts the SAME two heads via the SAME shared predicate. The lambda-
// body sibling of the method-level pair above; same reach, identical
// narrowing. The local-decl `var mk: (X) -> owned X := lambda(a: X) => a;`
// shape is the #4005-checked pattern (a lambda literal whose body returns
// the borrowed parameter directly through the target's `owned` return).
// ===========================================================================

BEZEL_TEST(shared_non_owning_iter_format_enumerable_head_predicate_4032,
           lambda_level_ienumerable_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(std::string(kMod) +
        "class C "
        "  public static method Main(): int "
        "    var mk: (IEnumerable<Cell>) -> owned IEnumerable<Cell> := "
        "      lambda(a: IEnumerable<Cell>) => a; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(shared_non_owning_iter_format_enumerable_head_predicate_4032,
           lambda_level_iasync_enumerable_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(std::string(kMod) +
        "class C "
        "  public static method Main(): int "
        "    var mk: (IAsyncEnumerable<Cell>) -> "
        "          owned IAsyncEnumerable<Cell> := "
        "      lambda(a: IAsyncEnumerable<Cell>) => a; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// REJECT — `checkOneLambdaOwnedReturnDirectBorrow` (Ownership.cpp / #4005)
// does NOT exempt any other iterator/enumerator head — same predicate
// (single shared source), same reach (just two heads), same reason
// (`IOwning*` extend IDisposable / IAsyncDisposable, stdlib §4.13).
// These four rows are the lambda-body twin of the four method-level
// REJECT rows above; together they prove neither site can silently
// reintroduce a wider hand-spelling without breaking this file.
// ===========================================================================

BEZEL_TEST(shared_non_owning_iter_format_enumerable_head_predicate_4032,
           lambda_level_ienumerator_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(std::string(kMod) +
        "class C "
        "  public static method Main(): int "
        "    var mk: (IEnumerator<Cell>) -> owned IEnumerator<Cell> := "
        "      lambda(a: IEnumerator<Cell>) => a; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(shared_non_owning_iter_format_enumerable_head_predicate_4032,
           lambda_level_iasync_enumerator_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(std::string(kMod) +
        "class C "
        "  public static method Main(): int "
        "    var mk: (IAsyncEnumerator<Cell>) -> "
        "          owned IAsyncEnumerator<Cell> := "
        "      lambda(a: IAsyncEnumerator<Cell>) => a; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(shared_non_owning_iter_format_enumerable_head_predicate_4032,
           lambda_level_iowning_enumerable_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(std::string(kMod) +
        "class C "
        "  public static method Main(): int "
        "    var mk: (IOwningEnumerable<Cell>) -> "
        "          owned IOwningEnumerable<Cell> := "
        "      lambda(a: IOwningEnumerable<Cell>) => a; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(shared_non_owning_iter_format_enumerable_head_predicate_4032,
           lambda_level_iowning_async_enumerable_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(std::string(kMod) +
        "class C "
        "  public static method Main(): int "
        "    var mk: (IOwningAsyncEnumerable<Cell>) -> "
        "          owned IOwningAsyncEnumerable<Cell> := "
        "      lambda(a: IOwningAsyncEnumerable<Cell>) => a; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}