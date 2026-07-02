// Fix #559 — Set<T>/OwningSet<T> collection literal `[e1, e2, …]` crashes the
// compiler with "IR verification failed: Invalid indices for GEP pointer type!".
//
// Root cause: `materializeHeapCollectionLiteral` returned nullptr for Set/OwningSet
// (intentional deferral), but `lowerLocalDecl` had no guard for that null and fell
// through to the inline-array GEP path, which treats the Set's `ptr` slot as a
// struct → malformed IR → verifier abort.
//
// Fix (LlvmBackend_Setup.cpp):
//   1. New helper `materializeHeapSetLiteral` allocates via bz_dict_alloc_generic
//      (same as `new Set<T>()`) and inserts each element via bz_dict_set_gen with
//      sentinel value 1 (same as Set.Add in tryLowerCollectionInstanceCall). Dedup
//      is inherent: bz_dict_set_gen on an already-present key is a no-op.
//   2. `materializeHeapCollectionLiteral` now routes Set/OwningSet through
//      `materializeHeapSetLiteral` instead of returning nullptr.
//   3. A safety guard in `lowerLocalDecl` (and the field-store path) prevents the
//      inline-array fallback for Set/OwningSet even if materialization returns null
//      (which it would for an unrecognised element type — a sema error already
//      diagnosed upstream).
//
// Spec authority: stdlib §4.10 (Set<T>/OwningSet<T>), spec §15.4 (collection
// literals), §4.10 dedup rule: "duplicates (per T's equality contract) are
// silently collapsed, the first textual occurrence retained".

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ===========================================================================
// §4.10 + §15.4 — Set<int> literal: core regression from #559
// ===========================================================================

// #559 / stdlib §4.10 / §15.4: the exact crash scenario from the issue report.
// Pre-fix: IR verification failure (compiler crash).  Post-fix: exit 1.
BEZEL_TEST(set_literal_559, set_int_contains_present_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s: Set<int> := [10, 20, 30]; "
        "  if 20 in s then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// #559 / stdlib §4.10 / §15.4: element absent in a Set<int> literal.
BEZEL_TEST(set_literal_559, set_int_contains_absent_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s: Set<int> := [10, 20, 30]; "
        "  if 99 in s then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// #559 / stdlib §4.10 / §15.4: Count of a Set<int> literal equals distinct count.
BEZEL_TEST(set_literal_559, set_int_count_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s: Set<int> := [10, 20, 30]; "
        "  return int(s.Count = 3); "
        "end Main; end C;"), 1);
}

// #559 / stdlib §4.10 / §15.4: empty Set<int> literal — Count == 0, Contains false.
BEZEL_TEST(set_literal_559, set_int_empty_literal_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s: Set<int> := []; "
        "  return int(s.Count = 0); "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §4.10 + §15.4 — dedup: duplicate elements in the literal are silently collapsed
// ===========================================================================

// #559 / stdlib §4.10 / §15.4: duplicates collapsed — Count is 2, not 3.
BEZEL_TEST(set_literal_559, set_int_dedup_count_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s: Set<int> := [1, 2, 2]; "
        "  return int(s.Count = 2); "
        "end Main; end C;"), 1);
}

// #559 / stdlib §4.10 / §15.4: all-duplicate literal yields Count 1.
BEZEL_TEST(set_literal_559, set_int_all_dupes_count_1_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s: Set<int> := [7, 7, 7]; "
        "  return int(s.Count = 1 and 7 in s); "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §4.10 + §15.4 — Set<string> literal (string elements are reference-counted)
// ===========================================================================

// #559 / stdlib §4.10 / §15.4 / §28.9: Set<string> literal with string elements.
// Verifies membership and Count; also exercises the #429 string-retain path.
BEZEL_TEST(set_literal_559, set_string_contains_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s: Set<string> := [\"hello\", \"world\", \"foo\"]; "
        "  return int(\"hello\" in s and \"world\" in s and not (\"bar\" in s)); "
        "end Main; end C;"), 1);
}

// #559 / stdlib §4.10 / §15.4 / §28.9: Set<string> literal dedup.
BEZEL_TEST(set_literal_559, set_string_dedup_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s: Set<string> := [\"a\", \"b\", \"a\"]; "
        "  return int(s.Count = 2); "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §4.10 + §15.4 — Set<T> literal with Add/Remove after construction
// ===========================================================================

// #559 / stdlib §4.10 / §15.4: Add after literal construction.
BEZEL_TEST(set_literal_559, set_int_add_after_literal_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s: Set<int> := [1, 2]; "
        "  s.Add(3); "
        "  return int(s.Count = 3 and 3 in s); "
        "end Main; end C;"), 1);
}

// #559 / stdlib §4.10 / §15.4: Remove after literal construction.
BEZEL_TEST(set_literal_559, set_int_remove_after_literal_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s: Set<int> := [10, 20, 30]; "
        "  s.Remove(20); "
        "  return int(s.Count = 2 and not (20 in s) and 10 in s and 30 in s); "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §4.10 + §15.4 — Set<T> with record element type
// ===========================================================================

// #559 / stdlib §4.10 / §15.4: Set<record> literal — record elements use
// genericBackingType (isSet=true), hash/eq trampolines, same as `new Set<P>()`.
// Uses positional-record shorthand `record P(x: int)` and `new P(n)` construction,
// matching the pattern that Set<record> `.Add()` tests already verify.
BEZEL_TEST(set_literal_559, set_record_literal_contains_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int); "
        "class C public static method Main(): int "
        "  using var s: Set<P> := [new P(1), new P(2), new P(3)]; "
        "  return int(s.Count = 3 and new P(2) in s and not (new P(9) in s)); "
        "end Main; end C;"), 1);
}

// #559 / stdlib §4.10 / §15.4: Set<record> literal dedup by structural equality.
BEZEL_TEST(set_literal_559, set_record_dedup_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int); "
        "class C public static method Main(): int "
        "  using var s: Set<P> := [new P(1), new P(2), new P(1)]; "
        "  return int(s.Count = 2); "
        "end Main; end C;"), 1);
}
