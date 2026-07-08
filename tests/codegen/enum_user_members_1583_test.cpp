// §10.5 (v0.183, #1583): a user-declared plain `Equals(E)`, `GetHashCode(): int`,
// or `ToString(): string` on an enum REPLACES the compiler-generated default for
// EVERY consumer — direct calls, string interpolation (§8.5), virtual dispatch
// through a boxed reference (§8.6), and hash containers (`Set<E>` /
// `Dictionary<E, V>` reach the user's Equals/GetHashCode, §4.8/§4.10). The
// compiler statically wires these members into the enum's box vtable; the wiring
// is well-defined because enums are sealed (no override chain, no `base`).
//
// Before #1583 the direct call already reached the user member, but interpolation
// and boxed dispatch missed the user `ToString` (rendered the type name) and the
// box vtable / hash-container trampolines used the default integer compare / hash.
//
// The JIT harness observes only the int exit code, so each string result is
// reduced to an int via an `=` comparison, and each bool/int result is returned
// directly (or through `1 if … else 0`).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// An enum whose user `ToString` maps High → "high".
const char* kPriorityToStr =
    "enum Priority Low, Medium, High; "
    "  public method ToString(): string => case this of "
    "    Low => \"low\", Medium => \"medium\", High => \"high\" end; "
    "end Priority; ";

// An enum whose user `Equals` treats all values as equal and whose user
// `GetHashCode` is a constant — so the user members are distinguishable from the
// §8.3 defaults (distinct values / per-value hash) at every consumer.
const char* kPriorityEqHash =
    "enum Priority Low, Medium, High; "
    "  public method Equals(other: Priority): bool => true; "
    "  public method GetHashCode(): int => 42; "
    "end Priority; ";

// A plain enum with NO user members — every consumer keeps the §8.5/§8.3 default.
const char* kColor = "enum Color Red := 0, Green := 1, Blue := 2; end Color; ";

} // namespace

// ── §10.5 / §8.5: user ToString reaches interpolation & boxed dispatch ───────

// Direct call already reached the user member (baseline).
BEZEL_TEST(codegen_enum_user_1583, tostring_direct_call) {
    BEZEL_EXPECT_EQ(run(std::string("module M; ") + kPriorityToStr +
        "class C public static method Main(): int "
        "  var p: Priority := Priority.High; "
        "  return (1 if p.ToString() = \"high\" else 0); "
        "end Main; end C;"), 1);
}

// §8.5: `$"{p}"` interpolates via the user ToString (was the type name "Priority").
BEZEL_TEST(codegen_enum_user_1583, tostring_interpolation) {
    BEZEL_EXPECT_EQ(run(std::string("module M; ") + kPriorityToStr +
        "class C public static method Main(): int "
        "  var p: Priority := Priority.High; "
        "  return (1 if $\"{p}\" = \"high\" else 0); "
        "end Main; end C;"), 1);
}

// §8.5: an aligned interpolation hole `$"{p,6}"` also uses the user ToString.
BEZEL_TEST(codegen_enum_user_1583, tostring_interpolation_aligned) {
    BEZEL_EXPECT_EQ(run(std::string("module M; ") + kPriorityToStr +
        "class C public static method Main(): int "
        "  var p: Priority := Priority.Medium; "
        "  return (1 if $\"{p,8}\" = \"  medium\" else 0); "
        "end Main; end C;"), 1);
}

// §8.6: virtual dispatch through a boxed Object reaches the user ToString
// (the enum box vtable's ToString slot; was the type name "Priority").
BEZEL_TEST(codegen_enum_user_1583, tostring_boxed_dispatch) {
    BEZEL_EXPECT_EQ(run(std::string("module M; ") + kPriorityToStr +
        "class C public static method Main(): int "
        "  var o: Object := Priority.High; "
        "  return (1 if o.ToString() = \"high\" else 0); "
        "end Main; end C;"), 1);
}

// ── §10.5 / §8.6: user Equals / GetHashCode reach boxed dispatch ─────────────

// §8.6: a boxed enum's `.Equals` reaches the user Equals — distinct values
// compare equal here because the user Equals returns true for all pairs (the
// §8.3 default would return false for Low vs High).
BEZEL_TEST(codegen_enum_user_1583, equals_boxed_dispatch) {
    BEZEL_EXPECT_EQ(run(std::string("module M; ") + kPriorityEqHash +
        "class C public static method Main(): int "
        "  var a: Object := Priority.Low; "
        "  var b: Object := Priority.High; "
        "  return (1 if a.Equals(b) else 0); "
        "end Main; end C;"), 1);
}

// §8.6: a boxed enum's `.GetHashCode` reaches the user GetHashCode (constant 42;
// the §8.3 default would hash the underlying integer 0 → 0).
BEZEL_TEST(codegen_enum_user_1583, gethashcode_boxed_dispatch) {
    BEZEL_EXPECT_EQ(run(std::string("module M; ") + kPriorityEqHash +
        "class C public static method Main(): int "
        "  var a: Object := Priority.Low; "
        "  return a.GetHashCode(); "
        "end Main; end C;"), 42);
}

// ── §10.5 / §4.10 / §4.8: user Equals / GetHashCode reach hash containers ────

// §4.10: a `Set<Priority>` reaches the user Equals/GetHashCode, so three
// "all-equal" values collapse to one element (the default keeps them distinct →
// Count 3).
BEZEL_TEST(codegen_enum_user_1583, set_uses_user_equality) {
    BEZEL_EXPECT_EQ(run(std::string("module M; ") + kPriorityEqHash +
        "class C public static method Main(): int "
        "  using var s: Set<Priority> := []; "
        "  s.Add(Priority.Low); s.Add(Priority.Medium); s.Add(Priority.High); "
        "  return s.Count; "
        "end Main; end C;"), 1);
}

// §4.8: a `Dictionary<Priority, int>` reaches the user Equals/GetHashCode, so
// three "all-equal" keys collapse to one entry (each assignment overwrites).
BEZEL_TEST(codegen_enum_user_1583, dictionary_uses_user_equality) {
    BEZEL_EXPECT_EQ(run(std::string("module M; ") + kPriorityEqHash +
        "class C public static method Main(): int "
        "  using var d: Dictionary<Priority, int> := []; "
        "  d[Priority.Low] := 1; d[Priority.Medium] := 2; d[Priority.High] := 3; "
        "  return d.Count; "
        "end Main; end C;"), 1);
}

// ── §10.5 negative: an enum WITHOUT user members keeps the §8.5/§8.3 defaults ─

// §8.5: a plain enum interpolates as its type name (no user ToString).
BEZEL_TEST(codegen_enum_user_1583, default_tostring_interpolation) {
    BEZEL_EXPECT_EQ(run(std::string("module M; ") + kColor +
        "class C public static method Main(): int "
        "  var g: Color := Color.Green; "
        "  return (1 if $\"{g}\" = \"Color\" else 0); "
        "end Main; end C;"), 1);
}

// §8.5: a plain enum's boxed ToString is its type name (no user ToString).
BEZEL_TEST(codegen_enum_user_1583, default_tostring_boxed_dispatch) {
    BEZEL_EXPECT_EQ(run(std::string("module M; ") + kColor +
        "class C public static method Main(): int "
        "  var o: Object := Color.Green; "
        "  return (1 if o.ToString() = \"Color\" else 0); "
        "end Main; end C;"), 1);
}

// §8.3: a plain enum's boxed Equals is the underlying-integer compare — distinct
// values compare unequal, identical values compare equal.
BEZEL_TEST(codegen_enum_user_1583, default_equals_boxed_dispatch) {
    BEZEL_EXPECT_EQ(run(std::string("module M; ") + kColor +
        "class C public static method Main(): int "
        "  var a: Object := Color.Red; var b: Object := Color.Blue; "
        "  var c: Object := Color.Red; var s := 0; "
        "  if a.Equals(b) then s := s + 10; end; "     // distinct → not equal
        "  if a.Equals(c) then s := s + 1; end; "      // same → equal
        "  return s; "
        "end Main; end C;"), 1);
}

// §4.10: a plain enum `Set` deduplicates by value — three distinct values plus a
// repeat yield Count 3 (the default equality/hash keep distinct values apart).
BEZEL_TEST(codegen_enum_user_1583, default_set_deduplicates_by_value) {
    BEZEL_EXPECT_EQ(run(std::string("module M; ") + kColor +
        "class C public static method Main(): int "
        "  using var s: Set<Color> := []; "
        "  s.Add(Color.Red); s.Add(Color.Green); s.Add(Color.Blue); "
        "  s.Add(Color.Red); "
        "  return s.Count; "
        "end Main; end C;"), 3);
}
