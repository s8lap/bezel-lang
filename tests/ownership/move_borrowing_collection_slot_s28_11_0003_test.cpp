// Spec-derived test for the rule-corpus row s28_11_0003
// (tests/conformance/rules/cases/s28_11_0003.bzl) — §28.11 (docs v0.307):
//
//   "Array<T>, Dictionary<K, V>, and the other borrowing collections do not
//   admit move coll[i] — their slots hold borrows, not owned values; there
//   is no ownership to transfer."
//
// `recordMovesIn`'s `move X[i]` handling (Ownership_Checks.cpp) already
// rejected `move` on a USER-DEFINED indexer getter (a `sema::PropertySymbol`
// with `isIndexer` — issue #4683's fix), but a BUILTIN borrowing collection's
// indexer (`Array<T>`, `Dictionary<K, V>`, ...) carries no PropertySymbol at
// all — it is compiler-intrinsic — so that check's `if (auto* ps =
// dynamic_cast<sema::PropertySymbol*>(...))` never matched it, and `move
// arr[0]` fell all the way through to a read-only `checkRead` + non-`move`
// `recordMovesIn` recursion, with NOTHING rejecting the transfer or marking
// any state change. MEASURED on the corpus case directly (FAIL-WITHOUT: the
// row read UNBANKED/pass-when-banked-reject pre-fix; green on the fix).
//
// per §28.11, v0.307

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

const char* kNeedle = "`move` is not admitted on a borrowing collection's slot";

bool rejectsWithNeedle(const std::string& program) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(kNeedle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// =============================================================================
// NEGATIVE — `move` on a builtin borrowing collection's indexer slot.
// =============================================================================

// The corpus case's exact shape: `move arr[0]` on an `Array<T>` local.
BEZEL_TEST(move_borrowing_collection_slot_0003, array_indexer_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWithNeedle(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Program "
        "  public static method Main() "
        "    const arr: Array<Widget> := [new Widget()]; "
        "    var w := move arr[0]; "
        "  end Main; "
        "end Program;"));
}

// `Dictionary<K, V>`'s value slot is the same borrowing-family shape (its
// non-key slot is what the rule addresses — §28.11's "the other borrowing
// collections").
BEZEL_TEST(move_borrowing_collection_slot_0003, dictionary_indexer_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWithNeedle(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Program "
        "  public static method Main() "
        "    const d: Dictionary<int, Widget> := [1: new Widget()]; "
        "    var w := move d[1]; "
        "  end Main; "
        "end Program;"));
}

// =============================================================================
// POSITIVE — no over-rejection: the owning-family counterpart still admits
// `move coll[i]` (that is the rule's own named exception).
// =============================================================================

BEZEL_TEST(move_borrowing_collection_slot_0003, owning_array_indexer_still_accepted) {
    auto r = runAot(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Program "
        "  public static method Main(): int "
        "    using var arr := new OwningArray<Widget>(); "
        "    arr.Add(new Widget()); "
        "    var w := move arr[0]; "
        "    w.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_TRUE(d.message.find(kNeedle) == std::string::npos);
}

// A plain (non-`move`) read of a borrowing collection's element is
// unaffected — the rule targets `move` specifically.
BEZEL_TEST(move_borrowing_collection_slot_0003, plain_array_read_still_accepted) {
    auto r = runAot(
        "module M; "
        "class Widget public V: int := 1; "
        "  public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Program "
        "  public static method Main(): int "
        "    const arr: Array<Widget> := [new Widget()]; "
        "    return arr[0].V; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
