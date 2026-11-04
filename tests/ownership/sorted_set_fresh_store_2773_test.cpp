// Spec-derived regression test for fix #2773 (B1) — `SortedSet<T>` joins the
// §15.4 fresh-store rule's borrowing-family whitelist. (The rule was a
// warning when this was written; since v0.346 (proposal 0207, #3047 lint A)
// it is a compile error, and the rows count the error.)
//
// BACKGROUND. Issue #3428 (v0.283) flagged, as a "knock-on" of its own
// rejection: `SortedSet` was absent from BOTH borrowing-family whitelists in
// `Ownership_Checks.cpp` (`checkCollectionBorrowStoreEscape` and
// `checkIndexerSetBorrowStoreEscape`), so `ss.Add(new Ord(1));` on a
// class-element `SortedSet<Ord>` would draw no §15.4 warning — but #3428 left
// the gap DELIBERATELY DORMANT, because no class-element `SortedSet` could be
// constructed at all: "the gap is currently unobservable... whoever fixes the
// construction defect ... should add SortedSet to those two whitelists in the
// same PR". `sorted_set_class_element_3428_test.cpp`'s
// (pre-#2773) `fresh_store_shape_still_unreachable` row pinned that dormancy.
//
// Fix #2773 (B1) makes construction succeed for a CompareTo-bearing
// class/interface element (see sorted_set_class_element_2773_test.cpp), so
// this row is the "in the same PR" half of #3428's instruction: `SortedSet`
// is added to both whitelists in the SAME commit that makes it reachable, and
// this test proves the warning actually fires rather than staying dormant.
//
// per §15.4 (fresh-store warning, v0.243), §28.11 *Borrowing collections*,
// stdlib §4.18 (v0.286): "the same ... value semantics ... as §4.17 apply" to
// `SortedSet<T>`, and §4.17's own values are borrowed exactly like every
// other borrowing-family collection's — so `SortedSet` belongs in the same
// whitelist as `Set`/`Array`/`Dictionary`, per #3428's own recommendation.

#include <string>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// `Ord: IComparable<Ord>` — the CompareTo-bearing class fix #2773 (B1) admits.
const char* kPreamble =
    "module M; "
    "class Ord: IComparable<Ord> "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  public method CompareTo(other: Ord): int return V - other.V; end CompareTo; "
    "end Ord; ";

OwnershipResult check(const std::string& body) {
    return parseAndCheckOwnership(std::string(kPreamble) + body);
}

// Counts the §15.4 fresh-store WARNINGs — same stable-rule-identifier
// convention as tests/ownership/fresh_store_warning_3364_test.cpp.
int freshStoreWarnings(const OwnershipResult& r) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("§15.4 fresh-store error") == std::string::npos)
            continue;
        ++n;
    }
    return n;
}

}  // namespace

// per §15.4 / stdlib §4.18 (v0.286), fix #2773 (B1): `SortedSet<Ord>.Add` of a
// fresh `new Ord(...)` temporary must draw the §15.4 fresh-store error — the exact
// shape #3428's "knock-on" section named and its own test pinned DORMANT.
BEZEL_TEST(sorted_set_fresh_store_2773, sortedset_add_fresh_temp_rejects) {
    OwnershipResult r = check(
        "class C public static method M(): int "
        "  using var s := new SortedSet<Ord>(); "
        "  s.Add(new Ord(3)); "
        "  return s.Count; "
        "end M; end C;");
    BEZEL_EXPECT_EQ(freshStoreWarnings(r), 1);
    // §15.4 (v0.346, proposal 0207; #3047 lint A): the store is a compile
    // ERROR at the store site (a warning before v0.346).
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// per §15.4 (v0.286): a NAMED holder draws no warning — the remedy §15.4
// itself names ("bind the source to a named holder first").
BEZEL_TEST(sorted_set_fresh_store_2773, sortedset_add_named_holder_not_rejected) {
    OwnershipResult r = check(
        "class C public static method M(): int "
        "  using var s := new SortedSet<Ord>(); "
        "  using var t := new Ord(3); "
        "  s.Add(t); "
        "  return s.Count; "
        "end M; end C;");
    BEZEL_EXPECT_EQ(freshStoreWarnings(r), 0);
}

// per §15.4 (v0.286): the indexer-set half of the shared sink
// (`checkIndexerSetBorrowStoreEscape`) is also widened, though `SortedSet`
// has no indexer (stdlib §4.18) to exercise it through — this row documents
// that absence rather than asserting a warning that cannot fire. `Add` is the
// only store position §4.18 declares, and it is covered above.
BEZEL_TEST(sorted_set_fresh_store_2773, sortedset_has_no_indexer_store_position) {
    OwnershipResult r = check(
        "class C public static method M(): int "
        "  using var s := new SortedSet<Ord>(); "
        "  s.Add(new Ord(3)); "
        "  return s.Count; "
        "end M; end C;");
    // Exactly the ONE `.Add` error — nothing from an indexer sink, because
    // `SortedSet<T>` (stdlib §4.18) declares no `[i: int]`/`[key: K]`
    // property for `checkIndexerSetBorrowStoreEscape` to ever match.
    BEZEL_EXPECT_EQ(freshStoreWarnings(r), 1);
}
