// Spec-derived regression tests for fix #3428 — `new SortedSet<T>()` with a
// REFERENCE element type is diagnosed instead of degrading to lowerLocalDecl's
// #2974 "produced no value" message.
//
// AUTHORITY. stdlib §4.18 (v0.283) declares, verbatim:
//
//     public class SortedSet<T>: IReadOnlyCollection<T> where T: IComparable<T>
//       public constructor();
//       ...
//     end SortedSet;
//
// under the heading sentence "A comparison-ordered set: membership by
// `CompareTo`-equality, iteration in ascending order.", and the prose adds
// "for `SortedSet<T>` that identity is `CompareTo`-equality, and adding an
// equal element leaves the set unmodified".
//
// THE DEFECT (measured on 0c5ecd1). A class `Ord: IComparable<Ord>` satisfies
// the sole constraint, yet
//
//     using var b := new SortedSet<Ord>();
//
// reported
//
//     `new SortedSet(…)` produced no value, so it cannot initialize this local.
//     `SortedSet` has no user declaration, so its CONSTRUCTOR surface is exactly
//     what the standard library documents for it: either `SortedSet` may not be
//     instantiated at all … or this argument list matches no documented
//     constructor (#2974)
//
// Both readings that message offers are FALSE — §4.18 documents a public
// parameterless constructor and the constraint is met — so it blamed the
// program for a compiler gap. The cause: codegen's `lowerNew` Set arm builds
// the generic-key hash table only when `genericBackingType(…, isSet=true)`
// returns an element type, and that helper deliberately excludes a class /
// interface element ("a class/interface SET element keeps its ARRAY-backed
// linear-scan machinery (emitArrayBackedSetInsert, #1070), a separate and
// working design"); `SortedSet` is then absent from the Array arm's name list,
// so the construction fell out of every branch and `lowerNew` returned nullptr.
//
// WHY THE FIX IS A REJECTION AND NOT AN IMPLEMENTATION. Routing the arm to the
// generic-key table — what `SortedDictionary` does for a class KEY, the #2773
// `hasObjectHeaderEquality` row — compiles, and breaks both halves of §4.18's
// contract silently:
//
//   * membership would key on `Equals`/`GetHashCode`, which for a class is §8.3
//     reference identity, so two `CompareTo`-equal instances would both be
//     stored where §4.18 says the set is left unmodified;
//   * iteration would not be ascending: the sorted-snapshot path needs
//     `buildSortedKeyComparator`, which returns null for anything that is not an
//     integer or float LLVM type. There is no `CompareTo`-through-the-vtable
//     comparator in this compiler.
//
// That is not a prediction. The issue calls `SortedDictionary<Ord, int>` the
// working control; measured on 0c5ecd1 it compiles and then iterates keys added
// 3, 1, 2 in INSERTION order (3, 1, 2 — not 1, 2, 3), and `FirstKey` SEGVs under
// ASan. Filed separately; a reference-element ordered collection needs a
// comparator-keyed backing, not this one. Per the #511 `GetType` / #2722
// comparer-constructor precedent the deferral is therefore made LOUD.
//
// A rejection is asserted on the diagnostic TEXT, never via a coarse
// `hasErrors()`: `runAot`'s WIFSIGNALED path synthesizes an `Error` diagnostic
// for a runtime SIGSEGV too, so a bare "did not run" cannot tell a genuine
// rejection from a crash — and `hasErrors()` cannot tell THIS rule from any
// other error in the program.
//
// The second half of this file is the FALSE-REJECT guard. Every element
// category the generic-key backing does support must still compile AND still
// produce the right answer, and the untouched `Set<T>` / `OwningSet<T>` arms
// must still accept a class element. Every row there compiles on the PRE-fix
// compiler too, which is what makes it a control rather than a restatement of
// the fix.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// The VALUE channel for an observable outside 0..255 — a process exit status is
// masked to 8 bits, so `1233` would arrive as `209`.
std::string out(const std::string& src) { return runAotOut(src); }

// A genuine compile-time rejection carrying `needle` in its message.
bool rejectsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// True when NO diagnostic in the compile carries `needle`.
bool lacks(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return false;
    return true;
}

// A class that satisfies §4.18's `where T: IComparable<T>` constraint, plus a
// record / enum for the value-element controls.
const char* PRELUDE =
    "module M; import System.Collections.Generic; "
    "record Rec(A: int); "
    "enum Col  Red, Green;  end Col; "
    "class Ord: IComparable<Ord> "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  public method CompareTo(other: Ord): int return V - other.V; end CompareTo; "
    "end Ord; ";

std::string prog(const std::string& body) {
    return std::string(PRELUDE) +
           "class C public static method Main(): int " + body +
           " end Main; end C;";
}

// The head of the new diagnostic — stable across the element categories.
const char* NEEDLE =
    "`SortedSet<T>` is not yet implemented for a reference element type";

}  // namespace

// ── The reference element categories are diagnosed (stdlib §4.18, v0.283) ────

// per stdlib §4.18 (v0.283), #3428: the filed repro, verbatim in shape. A CLASS
// element that implements `IComparable<T>` — the constraint §4.18 states — is
// exactly the case the old message called "matches no documented constructor".
BEZEL_TEST(sorted_set_class_element_3428, class_element_diagnosed) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "  using var b := new SortedSet<Ord>(); "
        "  return b.Count; "),
        NEEDLE));
}

// per stdlib §4.18 (v0.283), #3428: the message must name the element's
// category, so the reader can tell this from the sibling `Object` and interface
// rows without re-running the compiler.
BEZEL_TEST(sorted_set_class_element_3428, class_element_message_names_category) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "  using var b := new SortedSet<Ord>(); "
        "  return b.Count; "),
        "the element type here (`Ord`) is a class type"));
}

// per stdlib §4.18 (v0.283), #3428: an INTERFACE element reaches the same
// branch — `hasObjectHeaderEquality`, the predicate this fix reuses, admits
// "a class, an interface, or `Object`".
BEZEL_TEST(sorted_set_class_element_3428, interface_element_diagnosed) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "  using var b := new SortedSet<IComparable<Ord>>(); "
        "  return b.Count; "),
        "is an interface type"));
}

// per stdlib §4.18 (v0.283), #3428: `Object` is a Primitive-kind TypeRef, not a
// Named one, so it takes the other half of `hasObjectHeaderEquality` and would
// be missed by a `Kind::Named` test.
BEZEL_TEST(sorted_set_class_element_3428, object_element_diagnosed) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "  using var b := new SortedSet<Object>(); "
        "  return b.Count; "),
        "is the `Object` type"));
}

// per stdlib §4.18 (v0.283), #3428: the point of the fix is that the OLD,
// false message is gone. #2974's text asserts the constructor surface does not
// admit the call, which for §4.18's documented `public constructor();` is
// simply untrue.
BEZEL_TEST(sorted_set_class_element_3428, stale_2974_constructor_claim_gone) {
    BEZEL_EXPECT_TRUE(lacks(prog(
        "  using var b := new SortedSet<Ord>(); "
        "  return b.Count; "),
        "matches no documented constructor"));
}

// per stdlib §4.18 (v0.283), #3428: the §15.4 fresh-store shape the issue's
// "knock-on" section flags — `SortedSet` is absent from both fresh-store sinks'
// borrowing-family whitelists in Ownership_Checks.cpp. This row PINS that the
// gap stays DORMANT: the construction is rejected, so no class-element
// `SortedSet` exists for a fresh store to reach. If a later change makes
// construction succeed, this row goes red and the whitelists must be widened in
// the same commit (issues 3395 / 3364).
BEZEL_TEST(sorted_set_class_element_3428, fresh_store_shape_still_unreachable) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "  using var b := new SortedSet<Ord>(); "
        "  b.Add(new Ord(1)); "
        "  return b.Count; "),
        NEEDLE));
}

// ── FALSE-REJECT GUARD: every supported element category still works ─────────
//
// Each row below compiles and produces the same value on the PRE-fix compiler,
// which is what makes it a control. They cover the whole eligibility list
// `genericBackingType`'s set arm accepts: record, string, non-nullable
// primitive (int / char / double / decimal), enum.

// per stdlib §4.18 (v0.283), #3428: `int` elements — construct, dedup by
// value, and iterate in ascending order.
BEZEL_TEST(sorted_set_class_element_3428, int_element_still_sorted_and_dedups) {
    // The observable is 1233 — outside 0..255, so it travels on stdout rather
    // than the exit code, which WEXITSTATUS would mask to 209.
    BEZEL_EXPECT_EQ(out(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(3); s.Add(1); s.Add(3); s.Add(2); "
        "  var acc: int := 0; "
        "  for var x in s do acc := acc * 10 + x; end; "
        "  Console.WriteLine(acc * 10 + s.Count); "
        "  return 0; ")),
        /* 123 ascending, then Count 3 (the duplicate 3 dedups) */
        std::string("1233"));
}

// per stdlib §4.18 (v0.283), #3428: `string` elements — the D8 row of
// `genericBackingType`'s set arm (strings go generic for sets, unlike dicts).
BEZEL_TEST(sorted_set_class_element_3428, string_element_still_constructs) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<string>(); "
        "  s.Add(\"b\"); s.Add(\"a\"); s.Add(\"b\"); "
        "  return s.Count; ")),
        2);
}

// per stdlib §4.18 (v0.283), #3428: a RECORD element — structural equality, the
// oldest generic-key row (#343).
BEZEL_TEST(sorted_set_class_element_3428, record_element_still_constructs) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<Rec>(); "
        "  s.Add(new Rec(1)); s.Add(new Rec(2)); s.Add(new Rec(1)); "
        "  return s.Count; ")),
        2);
}

// per stdlib §4.18 (v0.283), #3428: an ENUM element (lowers to i32).
BEZEL_TEST(sorted_set_class_element_3428, enum_element_still_constructs) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<Col>(); "
        "  s.Add(Col.Red); s.Add(Col.Green); s.Add(Col.Red); "
        "  return s.Count; ")),
        2);
}

// per stdlib §4.18 (v0.283), #3428: `decimal` — the #685 row (a 16-byte struct
// with well-defined value equality), and `char`/`double`, so the guard covers
// the whole primitive width the arm admits rather than `int` alone.
BEZEL_TEST(sorted_set_class_element_3428, other_primitive_elements_still_construct) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var a := new SortedSet<decimal>(); a.Add(1.5m); a.Add(1.5m); "
        "  using var b := new SortedSet<char>();    b.Add('x');  b.Add('y'); "
        "  using var c := new SortedSet<double>();  c.Add(2.5);  c.Add(2.5); "
        "  return a.Count * 100 + b.Count * 10 + c.Count; ")),
        121);
}

// per stdlib §4.10 (v0.283), #3428: the arm this fix edits is shared with
// `Set<T>` / `OwningSet<T>`, whose CLASS element is legal and works today via
// the Array-backed linear scan (#1070). The rejection must not leak onto them.
BEZEL_TEST(sorted_set_class_element_3428, class_element_set_still_accepted) {
    // Deliberately NOT a re-add of the same reference. `Set<Ord>.Add(o1)` twice
    // was MEASURED (on 0c5ecd1, i.e. before this fix) to leave `Count == 3`
    // while `TryAdd(o1)` on the same set reports the element present — a
    // pre-existing §4.10 defect in the array-backed class-element path that this
    // commit neither causes nor repairs, filed separately. Pinning it here would
    // couple this guard to a bug. The row asserts only what is correct today and
    // must stay correct: two DISTINCT instances both land, and the set finds one.
    BEZEL_EXPECT_EQ(run(prog(
        "  using var o1 := new Ord(3); "
        "  using var o2 := new Ord(1); "
        "  var s := new Set<Ord>(); "
        "  s.Add(o1); s.Add(o2); "
        "  var n: int := s.Count; "
        "  if s.Contains(o1) then n := n + 10; end; "
        "  return n; ")),
        /* Count 2, and Contains(o1) is true */ 12);
}

// per stdlib §4.10 (v0.283), #3428: the `OwningSet<T>` half of the same arm.
BEZEL_TEST(sorted_set_class_element_3428, class_element_owning_set_still_accepted) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new OwningSet<Ord>(); "
        "  s.Add(new Ord(3)); s.Add(new Ord(1)); "
        "  return s.Count; ")),
        2);
}

// per stdlib §4.17 (v0.283), #3428: the `SortedDictionary<K, V>` arm is a
// DIFFERENT branch of `lowerNew` and is deliberately left alone by this commit
// — its class-key defects are filed separately. Pinning that it still compiles
// keeps this fix honest about its blast radius.
BEZEL_TEST(sorted_set_class_element_3428, sorted_dictionary_arm_untouched) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<int, int>(); "
        "  d.Add(3, 1); d.Add(1, 2); "
        "  return d.Count; ")),
        2);
}
