// fix #837 / stdlib §4.10 / §8.3: OwningSet<T> with a class element type that
// OVERRIDES Equals/GetHashCode must deduplicate by the user's equality contract,
// not by reference identity. The array-backed class-element set's TryAdd and Add
// compared element POINTERS, so a content-equal element was treated as distinct
// (TryAdd accepted it; Add appended it). Both now dispatch the element's
// equality via emitRemoveElementEquality (honouring a user `Equals` override),
// while a class WITHOUT an override keeps the §8.3 reference-identity default.
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/codegen_support.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
// A class with a user Equals/GetHashCode override keyed on field X.
const char* kBox =
    "module M; class Box "
    "  public static _n: int := 0; "
    "  public X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "  destructor _n := _n + 1; end destructor; "
    "  public override method Equals(other: Object?): bool "
    "    if other is Box then var b := other as Box; return b!.X = X; end; "
    "    return false; "
    "  end Equals; "
    "  public override method GetHashCode(): int => X; "
    "end Box; class C ";
// A class WITHOUT an Equals override (reference identity, §8.3 default).
const char* kPlain =
    "module M; class P "
    "  public X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "  destructor end destructor; "
    "end P; class C ";
}  // namespace

// TryAdd of a content-equal element is rejected (Count stays 2) — the bug14
// repro (was 3 by pointer identity).
BEZEL_TEST(owned_set_equality_837, tryadd_equal_rejected) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  do using var s := new OwningSet<Box>(); "
        "    s.Add(move (new Box(1))); s.Add(move (new Box(2))); "
        "    var r := s.TryAdd(move (new Box(1))); return s.Count; end; "
        "  return 0; end Main; end C;"), 2);
}

// Add of a content-equal element deduplicates (Count stays 1) — was 2.
BEZEL_TEST(owned_set_equality_837, add_equal_deduplicates) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  do using var s := new OwningSet<Box>(); "
        "    s.Add(move (new Box(1))); s.Add(move (new Box(1))); "
        "    return s.Count; end; "
        "  return 0; end Main; end C;"), 1);
}

// Add of content-DISTINCT elements keeps both.
BEZEL_TEST(owned_set_equality_837, add_distinct_kept) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  do using var s := new OwningSet<Box>(); "
        "    s.Add(move (new Box(1))); s.Add(move (new Box(2))); "
        "    return s.Count; end; "
        "  return 0; end Main; end C;"), 2);
}

// Contains uses the user Equals: a fresh content-equal query is found.
BEZEL_TEST(owned_set_equality_837, contains_by_value) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  do using var s := new OwningSet<Box>(); s.Add(move (new Box(5))); "
        "    using var q := new Box(5); "
        "    if s.Contains(q) then return 1; end; return 0; end; "
        "  return 0; end Main; end C;"), 1);
}

// The duplicate passed to Add is disposed (§4.10): 2 distinct in the set + 1
// duplicate disposed on Add = 3 destructor calls total.
BEZEL_TEST(owned_set_equality_837, add_duplicate_disposed) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  do using var s := new OwningSet<Box>(); "
        "    s.Add(move (new Box(1))); s.Add(move (new Box(2))); "
        "    s.Add(move (new Box(1))); end; "
        "  return Box._n; end Main; end C;"), 3);
}

// The duplicate passed to TryAdd is disposed (§4.10): same count = 3.
BEZEL_TEST(owned_set_equality_837, tryadd_duplicate_disposed) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  do using var s := new OwningSet<Box>(); "
        "    s.Add(move (new Box(1))); s.Add(move (new Box(2))); "
        "    var r := s.TryAdd(move (new Box(1))); end; "
        "  return Box._n; end Main; end C;"), 3);
}

// Regression: a class WITHOUT an Equals override keeps reference identity — two
// distinct instances with the same field are both retained (Count = 2).
BEZEL_TEST(owned_set_equality_837, no_override_reference_identity) {
    BEZEL_EXPECT_EQ(run(std::string(kPlain) +
        "public static method Main(): int "
        "  do using var s := new OwningSet<P>(); "
        "    s.Add(move (new P(1))); s.Add(move (new P(1))); "
        "    return s.Count; end; "
        "  return 0; end Main; end C;"), 2);
}

// Regression: OwningArray.Add still appends (no set dedup).
BEZEL_TEST(owned_set_equality_837, owned_array_add_still_appends) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  do using var a := new OwningArray<Box>(); "
        "    a.Add(move (new Box(1))); a.Add(move (new Box(1))); "
        "    return a.Count; end; "
        "  return 0; end Main; end C;"), 2);
}
