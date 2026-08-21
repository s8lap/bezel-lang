// Spec-derived tests for issue #2667 — §28.9's trivial-dispose exemption from
// the §28.3 ownership-discharge (auto-drop must-use) WARNING extended to the
// BUILTIN borrowing-collection class heads. Spec v0.268.
//
// §28.9 "Trivial-dispose for generic class instantiations", VERBATIM:
//
//     "The standard collection types — `Array<T>`, `Dictionary<K, V>`,
//      `Set<T>`, `LinkedList<T>`, `Stack<T>`, `Queue<T>` — are trivial-dispose
//      for every non-owned choice of type argument because they do not own
//      their elements."
//
// §28.9 next paragraph: "The owned-element variants of the standard collection
// types — `OwningArray<T>`, `OwningDictionary<K, V>`, `OwningSet<T>`, etc. —
// are NOT trivial-dispose at any instantiation: they own their elements,
// disposal cascades through to the elements, and their storage primitive
// declares a destructor".
//
// Before the fix, `localIsTrivialDisposeNewBinding` (Ownership.cpp:5289,
// #2654) bailed at the `if (!d || d->kind != ast::TypeDeclKind::Class)` check
// when `t->declaration` was null — which it is for EVERY builtin class head,
// since `Array<int>`, `Dictionary<K,V>`, etc. have no user TypeDecl. The
// builtin heads therefore kept today's §28.3 warning even where §28.9 says
// they shouldn't. Fixed by extending the predicate to consult the SHARED
// `common::isChainClaimedBuiltinHeadName` table (BuiltinRegistry.cpp:555-558,
// #3017) — 8 borrowing collections (Array/Stack/Queue/Set/SortedSet/
// Dictionary/SortedDictionary/LinkedList) plus 5 vtable-less trivial-dispose
// builtins (Random/Atomic/AtomicInteger/Stopwatch/Buffer). The Owning*
// variants are DELIBERATELY absent from the table and must keep the warning.
//
// ⚠ SCOPE OF THE FIX — same separation as #2654: the fix suppresses the
// WARNING ONLY. `LocalDecl::autoDropAtScopeEnd` is still set above (line 748,
// #2654), so codegen still emits exactly the same scope-end free — including
// for the borrowing collections, whose chain-family arms (registerUsingDisposal,
// LlvmBackend_Setup.cpp) reclaim the backing. Dropping the free instead of the
// diagnostic would leak; dropping it on a shared-borrow shape would double-free.
// See `localIsTrivialDisposeNewBinding`'s own header comment for the rationale.
//
// The Owning* / OwningBuffer NEGATIVE cases are the false-negative controls:
// a suppression bug that swallowed the warning for genuinely non-trivial
// owning variants would hide a real §28.3 element-disposal obligation, which
// is the more dangerous direction.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

std::string wrap(const std::string& decls, const std::string& mainBody) {
    return std::string("module M; ") + decls +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// =============================================================================
// POSITIVE — the §28.9 exemption extends to the builtin borrowing collections.
// =============================================================================

// §28.9 trivially-disposes `Array<T>` "for every non-owned choice of type
// argument because [it does] not own [its] elements". `var a := new Array<int>();`
// is the issue's primary repro. // fix #2667
BEZEL_TEST(own_discharge_warning_exemption_builtin_2667,
           array_borrowing_is_exempt) {
    auto r = parseAndCheckOwnership(wrap(
        "",
        "var a := new Array<int>(); return a.Count;"));
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "a"));
}

BEZEL_TEST(own_discharge_warning_exemption_builtin_2667,
           dictionary_borrowing_is_exempt) {
    auto r = parseAndCheckOwnership(wrap(
        "",
        "var b := new Dictionary<int, int>(); return b.Count;"));
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "b"));
}

BEZEL_TEST(own_discharge_warning_exemption_builtin_2667,
           set_borrowing_is_exempt) {
    auto r = parseAndCheckOwnership(wrap(
        "",
        "var s := new Set<int>(); return s.Count;"));
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "s"));
}

BEZEL_TEST(own_discharge_warning_exemption_builtin_2667,
           linkedlist_borrowing_is_exempt) {
    auto r = parseAndCheckOwnership(wrap(
        "",
        "var l := new LinkedList<int>(); return l.Count;"));
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "l"));
}

BEZEL_TEST(own_discharge_warning_exemption_builtin_2667,
           stack_borrowing_is_exempt) {
    auto r = parseAndCheckOwnership(wrap(
        "",
        "var k := new Stack<int>(); return k.Count;"));
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "k"));
}

BEZEL_TEST(own_discharge_warning_exemption_builtin_2667,
           queue_borrowing_is_exempt) {
    auto r = parseAndCheckOwnership(wrap(
        "",
        "var q := new Queue<int>(); return q.Count;"));
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "q"));
}

// §4.18 (#1426): SortedSet<T> shares the generic-key hash table with Set<T>
// and is BORROWING-family (does not own elements) — it joins the
// `isChainClaimedBuiltinHeadName` table even though the §28.9 sentence names
// only the 6 most-used collections. Without this test, a future spec edit
// removing the Set/Array/Dictionary/LinkedList/Stack/Queue clause could leave
// SortedSet silently warning — and the §28.9 underlying rule ("they do not own
// their elements") covers it. // fix #2667
BEZEL_TEST(own_discharge_warning_exemption_builtin_2667,
           sortedset_borrowing_is_exempt) {
    auto r = parseAndCheckOwnership(wrap(
        "",
        "var s := new SortedSet<int>(); return s.Count;"));
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "s"));
}

BEZEL_TEST(own_discharge_warning_exemption_builtin_2667,
           sorteddictionary_borrowing_is_exempt) {
    auto r = parseAndCheckOwnership(wrap(
        "",
        "var d := new SortedDictionary<int, int>(); return d.Count;"));
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "d"));
}

// §28.9 also covers the vtable-less trivial-dispose builtins on the
// chain-claimed set (Random / Atomic / AtomicInteger / Stopwatch / Buffer) —
// each has no destructor and no `owned` fields, so each is trivial-dispose by
// structural rule and the free at scope end runs no user code. // fix #2667
BEZEL_TEST(own_discharge_warning_exemption_builtin_2667,
           random_is_exempt) {
    auto r = parseAndCheckOwnership(wrap(
        "",
        "var r := new Random(); return 0;"));
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "r"));
}

// =============================================================================
// NEGATIVE — the §28.9 exemption does NOT extend to the Owning* / OwningBuffer
// variants. Each is non-trivial-dispose by §28.9's own paragraph ("NOT
// trivial-dispose at any instantiation: they own their elements… and their
// storage primitive declares a destructor"), so the §28.3 warning MUST keep
// firing. Without these, a future broadness bug in the chain-claimed-table
// consult would silently swallow the warning on owning-collection locals,
// hiding a real element-disposal obligation.
// =============================================================================

// §28.9: "The owned-element variants of the standard collection types —
// `OwningArray<T>`, … — are NOT trivial-dispose at any instantiation".
BEZEL_TEST(own_discharge_warning_exemption_builtin_2667,
           owning_array_still_warns) {
    auto r = parseAndCheckOwnership(wrap(
        "class Holder public constructor() end constructor; "
        "  public V: int := 0; end Holder; ",
        "var own := new OwningArray<Holder>(4, 0); return own.Count;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "own"));
}

BEZEL_TEST(own_discharge_warning_exemption_builtin_2667,
           owning_dictionary_still_warns) {
    auto r = parseAndCheckOwnership(wrap(
        "class Holder public constructor() end constructor; "
        "  public V: int := 0; end Holder; ",
        "var own := new OwningDictionary<int, Holder>(); return own.Count;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "own"));
}

BEZEL_TEST(own_discharge_warning_exemption_builtin_2667,
           owning_set_still_warns) {
    auto r = parseAndCheckOwnership(wrap(
        "class Holder public constructor() end constructor; "
        "  public V: int := 0; end Holder; ",
        "var own := new OwningSet<Holder>(); return own.Count;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "own"));
}

BEZEL_TEST(own_discharge_warning_exemption_builtin_2667,
           owning_buffer_still_warns) {
    auto r = parseAndCheckOwnership(wrap(
        "",
        "var own := new OwningBuffer<double>(4); return own.Length;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "own"));
}
