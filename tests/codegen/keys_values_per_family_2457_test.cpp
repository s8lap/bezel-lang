// Regression tests for issue #2457 — the `Keys`/`Values` per-family
// false-accept. #2536 (c7eb5b5) added `Keys`/`Values` to the
// `isKnownProperty` allow-list of the §27.6 "no member" catch-all for the
// eight declaration-less builtin collection families (sema/
// ExprTypecheck_Calls.cpp's #2536 block) — but the list was UNCONDITIONAL,
// so a bare reference to `Keys`/`Values` on a family that does NOT declare
// those properties compiled silently. Confirmed live on `origin/main`
// before this fix:
//
//   var sd := new SortedDictionary<string, int>(); sd.Add("a", 1);
//   var k := sd.Keys;        // §4.17 SortedDictionary has NO Keys/Values
//                            // (only FirstKey/LastKey) — should REJECT
//
// compiled at exit 0 with no diagnostic and no sema-typed result (the
// property handler at ~10908 covers Dictionary/OwningDictionary only; the
// catch-all here exempted the name from the "no member" reject; codegen's
// #2434 zero-init fallback then degraded the local to `i32`). The same
// silent-accept shape applies to SortedSet (§4.18: no Keys/Values),
// Set/OwningSet (§4.10: sets, not maps), and LinkedList/OwningLinkedList
// (§4.6/§4.7: lists, not maps) — six of the eight families in the catch-all
// range. Dictionary (§4.8) and OwningDictionary (§4.9) DO declare Keys/
// Values and remain legitimate.
//
// This is a small, targeted slice of the broader #2457 umbrella (which
// still tracks ToString/Equals/GetHashCode implementation for these
// families and the span/buffer family — out of scope here).
//
// Per stdlib verification (not inferred from what the compiler special-cases):
//   - Dictionary (§4.8): declares `Keys: IReadOnlyCollection<K>` /
//     `Values: IReadOnlyCollection<V>`. KEPT in
//     isKnownProperty.
//   - OwningDictionary (§4.9): declares the same. KEPT.
//   - SortedDictionary (§4.17): declares Count, [key], ContainsKey,
//     Contains, TryGetValue, GetOrNull, Remove, Clear, FirstKey, LastKey —
//     NO Keys/Values. REMOVED from isKnownProperty (now correctly
//     rejected as a "no member" by the catch-all).
//   - SortedSet (§4.18): declares Count, Add, TryAdd, Contains, Remove,
//     Clear, First, Last — NO Keys/Values. REMOVED.
//   - Set/OwningSet (§4.10): declares ICollection/IOwningCollection
//     operations + TryAdd/UnionWith/etc. — NO Keys/Values. REMOVED.
//   - LinkedList/OwningLinkedList (§4.6/§4.7): declares Count, FirstNode/
//     LastNode, First/Last, AddFirst/AddLast/etc. — NO Keys/Values. REMOVED.
//   - `Count` is in the documented surface of every one of the eight
//     families (§4.7-§4.10, §4.17, §4.18) — UNCHANGED, still allowed
//     unconditionally.
//
// The fix narrows the isKnownProperty check from
//   mbr == "Count" || mbr == "Keys" || mbr == "Values"
// to
//   mbr == "Count" ||
//   ((mbr == "Keys" || mbr == "Values") &&
//    (cn == "Dictionary" || cn == "OwningDictionary"))
// — converting the silent false-accept on the six non-map families into a
// proper "no member" reject. Dictionary and OwningDictionary are unchanged.
// `Count` is unchanged (every family documents it).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must
// be distinguished by asserting on the diagnostic TEXT, never by exit code
// or `!r.ran` alone.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// ── `Keys`/`Values` rejected on families that don't declare them ──────

// SortedDictionary (§4.17): declares FirstKey/LastKey but NOT Keys/Values.
BEZEL_TEST(keys_values_per_family_2457, keys_rejected_on_sorteddictionary_stdlib_4_17) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var sd := new SortedDictionary<string, int>(); "
        "  sd.Add(\"a\", 1); "
        "  var k := sd.Keys; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "no member `Keys` on `SortedDictionary`"));
}

BEZEL_TEST(keys_values_per_family_2457, values_rejected_on_sorteddictionary_stdlib_4_17) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var sd := new SortedDictionary<string, int>(); "
        "  sd.Add(\"a\", 1); "
        "  var v := sd.Values; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "no member `Values` on `SortedDictionary`"));
}

// SortedSet (§4.18): declares Count/Add/TryAdd/Contains/Remove/Clear/
// First/Last — NOT Keys/Values.
BEZEL_TEST(keys_values_per_family_2457, keys_rejected_on_sortedset_stdlib_4_18) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var ss := new SortedSet<int>(); "
        "  ss.Add(1); "
        "  var k := ss.Keys; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "no member `Keys` on `SortedSet`"));
}

// Set (§4.10): a set, not a map — no Keys/Values declared.
BEZEL_TEST(keys_values_per_family_2457, keys_rejected_on_set_stdlib_4_10) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := new Set<int>(); "
        "  s.Add(1); "
        "  var k := s.Keys; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "no member `Keys` on `Set`"));
}

BEZEL_TEST(keys_values_per_family_2457, values_rejected_on_set_stdlib_4_10) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := new Set<int>(); "
        "  s.Add(1); "
        "  var v := s.Values; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "no member `Values` on `Set`"));
}

// OwningSet (§4.10): a set, not a map — V must be class per §4.2/§14.4
// (#1327), so probe uses a class-typed element. Bare ref to Keys still
// must be rejected.
BEZEL_TEST(keys_values_per_family_2457, keys_rejected_on_owningset_stdlib_4_10) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  var os := new OwningSet<Res>(); "
        "  os.Add(new Res(1)); "
        "  var k := os.Keys; "
        "  os.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "no member `Keys` on `OwningSet`"));
}

// LinkedList (§4.6): a list, not a map — no Keys/Values declared.
BEZEL_TEST(keys_values_per_family_2457, keys_rejected_on_linkedlist_stdlib_4_6) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var ll := new LinkedList<int>(); "
        "  ll.AddFirst(1); "
        "  var k := ll.Keys; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "no member `Keys` on `LinkedList`"));
}

BEZEL_TEST(keys_values_per_family_2457, values_rejected_on_linkedlist_stdlib_4_6) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var ll := new LinkedList<int>(); "
        "  ll.AddFirst(1); "
        "  var v := ll.Values; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "no member `Values` on `LinkedList`"));
}

// OwningLinkedList (§4.7): a list, not a map — V must be class per §4.2/
// §14.4 (#1327).
BEZEL_TEST(keys_values_per_family_2457, keys_rejected_on_owninglinkedlist_stdlib_4_7) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  var oll := new OwningLinkedList<Res>(); "
        "  oll.AddFirst(new Res(1)); "
        "  var k := oll.Keys; "
        "  oll.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "no member `Keys` on `OwningLinkedList`"));
}

// ── `Keys`/`Values` STILL work on the two families that DO declare them ─

// Dictionary<K,V> (§4.8): declares Keys: IReadOnlyCollection<K> / Values:
// IReadOnlyCollection<V>. Bare reference to Keys MUST continue to compile
// and produce a working value (the property handler at ~10908 types it as
// a synthetic Array<K>; iteration via for-in returns the K's in some
// sensible order). Adversarial: VALUES-correct, not just "compiles".
BEZEL_TEST(keys_values_per_family_2457, keys_still_work_on_dictionary_stdlib_4_8) {
    const std::string program =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  var d := new Dictionary<string, int>(); "
        "  d.Add(\"a\", 1); d.Add(\"b\", 2); "
        "  var k := d.Keys; "
        "  var n := 0; "
        "  for var key in k do n := n + 1; end; "
        "  d.Dispose(); "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 2);
}

BEZEL_TEST(keys_values_per_family_2457, values_still_work_on_dictionary_stdlib_4_8) {
    const std::string program =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  var d := new Dictionary<string, int>(); "
        "  d.Add(\"a\", 10); d.Add(\"b\", 20); "
        "  var v := d.Values; "
        "  var sum := 0; "
        "  for var val in v do sum := sum + val; end; "
        "  d.Dispose(); "
        "  return sum; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 30);
}

// OwningDictionary<K,V> (§4.9): V must be class. Bare reference to Keys
// MUST still compile and iterate.
BEZEL_TEST(keys_values_per_family_2457, keys_still_work_on_owningdictionary_stdlib_4_9) {
    const std::string program =
        "module M; import System.Collections.Linq; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  var od := new OwningDictionary<string, Res>(); "
        "  od.Add(\"a\", new Res(1)); od.Add(\"b\", new Res(2)); "
        "  var k := od.Keys; "
        "  var n := 0; "
        "  for var key in k do n := n + 1; end; "
        "  od.Dispose(); "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 2);
}

// ── `Count` unchanged: every one of the eight families still allows it ──

// Count is in the stdlib-documented surface of every one of the eight
// families (§4.7-§4.10, §4.17, §4.18) — must remain accepted for all of
// them, including the six the Keys/Values fix above narrows for. Sampled
// for direct guard on this fix's isKnownProperty change; the broad
// `dict_`/`set_`/`linkedlist_` suites cover the full call surface.
BEZEL_TEST(keys_values_per_family_2457, count_still_works_on_all_eight_families) {
    // Dictionary: borrowing, no Dispose.
    {
        const std::string program =
            "module M; "
            "class C public static method Main(): int "
            "  var d := new Dictionary<string, int>(); "
            "  d.Add(\"a\", 1); d.Add(\"b\", 2); "
            "  return d.Count; "
            "end Main; end C;";
        BEZEL_EXPECT_EQ(run(program), 2);
    }
    // OwningDictionary: owning, V must be class.
    {
        const std::string program =
            "module M; "
            "class Res public Tag: int; "
            "  public constructor(t: int) Tag := t; end constructor; "
            "end Res; "
            "class C public static method Main(): int "
            "  var od := new OwningDictionary<string, Res>(); "
            "  od.Add(\"a\", new Res(1)); "
            "  var n := od.Count; "
            "  od.Dispose(); "
            "  return n; "
            "end Main; end C;";
        BEZEL_EXPECT_EQ(run(program), 1);
    }
    // SortedDictionary.
    {
        const std::string program =
            "module M; "
            "class C public static method Main(): int "
            "  var sd := new SortedDictionary<string, int>(); "
            "  sd.Add(\"a\", 1); sd.Add(\"b\", 2); "
            "  return sd.Count; "
            "end Main; end C;";
        BEZEL_EXPECT_EQ(run(program), 2);
    }
    // SortedSet.
    {
        const std::string program =
            "module M; "
            "class C public static method Main(): int "
            "  var ss := new SortedSet<int>(); "
            "  ss.Add(1); ss.Add(2); "
            "  return ss.Count; "
            "end Main; end C;";
        BEZEL_EXPECT_EQ(run(program), 2);
    }
    // Set.
    {
        const std::string program =
            "module M; "
            "class C public static method Main(): int "
            "  var s := new Set<int>(); "
            "  s.Add(1); s.Add(2); "
            "  return s.Count; "
            "end Main; end C;";
        BEZEL_EXPECT_EQ(run(program), 2);
    }
    // OwningSet.
    {
        const std::string program =
            "module M; "
            "class Res public Tag: int; "
            "  public constructor(t: int) Tag := t; end constructor; "
            "end Res; "
            "class C public static method Main(): int "
            "  var os := new OwningSet<Res>(); "
            "  os.Add(new Res(1)); os.Add(new Res(2)); "
            "  var n := os.Count; "
            "  os.Dispose(); "
            "  return n; "
            "end Main; end C;";
        BEZEL_EXPECT_EQ(run(program), 2);
    }
    // LinkedList.
    {
        const std::string program =
            "module M; "
            "class C public static method Main(): int "
            "  var ll := new LinkedList<int>(); "
            "  ll.AddFirst(1); ll.AddFirst(2); "
            "  return ll.Count; "
            "end Main; end C;";
        BEZEL_EXPECT_EQ(run(program), 2);
    }
    // OwningLinkedList.
    {
        const std::string program =
            "module M; "
            "class Res public Tag: int; "
            "  public constructor(t: int) Tag := t; end constructor; "
            "end Res; "
            "class C public static method Main(): int "
            "  var oll := new OwningLinkedList<Res>(); "
            "  oll.AddFirst(new Res(1)); oll.AddFirst(new Res(2)); "
            "  var n := oll.Count; "
            "  oll.Dispose(); "
            "  return n; "
            "end Main; end C;";
        BEZEL_EXPECT_EQ(run(program), 2);
    }
}
