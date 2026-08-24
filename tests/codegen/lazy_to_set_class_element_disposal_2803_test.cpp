// Regression test for #2803 — `IEnumerable<T>.ToSet()` over a class/interface
// element built a DICT-backed `Set<T>` while the element's own `Set<T>`/
// `OwningSet<T>` is ARRAY-backed (spec §4.10 / §28.9), so the disposal site
// freed a `bz_array_t*` that the build path had populated as a `bz_dict_t*`.
// Measured (pre-fix, ASan/LSan): 80 bytes / 3 allocations leaked per ToSet()
// over a class element.
//
// THE FIX. The runtime's `bz_lazy_to_set_arraybacked` drains the lazy into a
// `bz_array_t` (the same shape `new Set<C>()` builds), with a linear
// `eqfn`-driven dedup. Codegen dispatches to it whenever the element's
// `genericBackingType` returns null (class/interface), keeping the existing
// dict-backed path for every other element type.
//
// Fails-without / passes-with: before the fix, `runAotChecked` flips r.ran
// to false (LSan reports the leak, ASan aborts). After the fix every program
// exits cleanly with the expected element count, ASan/LSan-clean.
//
// Spec: §4.10 (Set stdlib surface), §28.9 (Runtime-managed types), §8.3
// (Equality contracts). Revision v0.240.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

const char* kKey =
    "module M; import System; "
    "class Key "
    "  public X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "  public override method Equals(other: Object?): bool "
    "    if other is Key then var k := other as Key; return k!.X = X; end; "
    "    return false; "
    "  end Equals; "
    "  public override method GetHashCode(): int => X; "
    "end Key; ";

const char* kShape =
    "module M; import System; "
    "interface IShape method Area(): int; end IShape; "
    "class Sq: IShape "
    "  public S: int; "
    "  public constructor(s: int) S := s; end constructor; "
    "  public method Area(): int => S * S; "
    "end Sq; ";

std::string main_(const char* fixture, const std::string& body) {
    return std::string(fixture) + "class App public static method Main(): int " +
           body + " end Main; end App;";
}

}  // namespace

// The pin — class element ToSet() must round-trip cleanly. Before the fix:
// LSan reports "80 byte(s) leaked in 3 allocation(s)" from dict_order_append
// and bz_dict_set_gen (r.ran=false).
BEZEL_TEST(codegen_lazy_to_set_class_element_disposal_2803,
           class_element_with_equals_override_spec_4_10) {
    BEZEL_EXPECT_EQ(run(main_(kKey,
        "  using var k0 := new Key(1); "
        "  using var k1 := new Key(1); "
        "  using var k2 := new Key(2); "
        "  using var a := new Array<Key>(); "
        "  a.Add(k0); a.Add(k1); a.Add(k2); "
        "  using var s := a.ToSet(); "
        "  return s.Count;")), 2);
}

// Interface element with reference identity (no override). Three `Sq`s stay
// three distinct elements.
BEZEL_TEST(codegen_lazy_to_set_class_element_disposal_2803,
           interface_element_spec_4_10) {
    BEZEL_EXPECT_EQ(run(main_(kShape,
        "  using var s0 := new Sq(1); "
        "  using var s1 := new Sq(2); "
        "  using var s2 := new Sq(1); "
        "  var a: Array<IShape> := [s0, s1, s2]; "
        "  using var ss := a.ToSet(); "
        "  return ss.Count;")), 3);
}

// Repeat the class-element ToSet() several times to amplify the leak signal
// (LSan is layout-sensitive; repeated allocation makes a single miss easier
// to observe). 2 distinct elements * 8 rounds = 16.
BEZEL_TEST(codegen_lazy_to_set_class_element_disposal_2803,
           class_element_repeated_spec_4_10) {
    std::string body =
        "  using var k0 := new Key(1); "
        "  using var k1 := new Key(2); "
        "  using var k2 := new Key(1); "
        "  using var a := new Array<Key>(); "
        "  a.Add(k0); a.Add(k1); a.Add(k2); "
        "  var total := 0; "
        "  var i := 0; "
        "  while i < 8 do "
        "    using var s := a.ToSet(); "
        "    total := total + s.Count; "
        "    i := i + 1; "
        "  end; "
        "  return total; ";
    BEZEL_EXPECT_EQ(run(main_(kKey, body)), 16);
}

// Over-correction controls — every element shape that was NEVER broken must
// keep its existing behavior. The dispatch is gated on genericBackingType
// returning null (class/interface only).

// int element — genericBackingType returns non-null, dict path runs.
BEZEL_TEST(codegen_lazy_to_set_class_element_disposal_2803,
           int_element_still_dict_backed_spec_4_10) {
    BEZEL_EXPECT_EQ(run(main_(
        "module M; import System; ",
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(1); a.Add(3); "
        "  using var s := a.ToSet(); "
        "  return s.Count;")), 3);
}

// Auto-hashable record element — genericBackingType returns non-null.
BEZEL_TEST(codegen_lazy_to_set_class_element_disposal_2803,
           record_element_still_dict_backed_spec_4_10) {
    BEZEL_EXPECT_EQ(run(main_(
        "module M; import System; record Pt(x: int); ",
        "  using var a := new Array<Pt>(); "
        "  a.Add(new Pt(1)); a.Add(new Pt(2)); a.Add(new Pt(1)); "
        "  using var s := a.ToSet(); "
        "  return s.Count;")), 2);
}

// string element — genericBackingType returns non-null (string fast path).
BEZEL_TEST(codegen_lazy_to_set_class_element_disposal_2803,
           string_element_still_dict_backed_spec_4_10) {
    BEZEL_EXPECT_EQ(run(main_(
        "module M; import System; ",
        "  using var a := new Array<string>(); "
        "  a.Add(\"apple\"); a.Add(\"banana\"); a.Add(\"apple\"); "
        "  using var s := a.ToSet(); "
        "  return s.Count;")), 2);
}
