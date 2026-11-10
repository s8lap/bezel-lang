// Spec-derived regression test for issue #8331 (stdlib §1 *Block-by-name*,
// §4.17, §4.19, v0.355).
//
// THE DEFECT. A stdlib collection head still admitted members that NO stdlib
// section declares for it:
//
//   | head                | member  | the section that declares the head | declares it? |
//   |---------------------|---------|-------------------------------------|--------------|
//   | `FrozenArray<T>`    | `Add`   | §4.19                                | no           |
//   | `FrozenArray<T>`    | `Clear` | §4.19                                | no           |
//   | `FrozenArray<T>`    | `Remove`| §4.19                                | no           |
//   | `FrozenArray<T>`    | `Insert`| §4.19                                | no           |
//   | `SortedDictionary<K,V>` | `Insert` | §4.17                            | no           |
//
// All five COMPILED. `FrozenArray<T>`'s four compiled SILENTLY (no
// diagnostic at all): §4.19 declares NO mutator at all — the type is the
// stdlib's deeply-immutable, fixed-size array ("`FrozenArray<T>` has no
// mutating member"), and sema's call-surface gate did not apply to the head
// at all, so codegen's array arms lowered the call as if the head were
// `Array<T>`. `SortedDictionary<K, V>.Insert` compiled with only the §21.9
// "the result of this expression statement is unused" warning — the call
// itself lowered to nothing.
//
// THE ROOT CAUSE (both SEMA-side, per stdlib §1's Block-by-name rule: "a
// member a type does not declare is not a member of that type at all"):
//
//   1. `FrozenArray` was in NEITHER of sema's container tables
//      (`isOwningContainer` / `isNonOwningContainer`), so NO call-surface
//      gate applied to it at all;
//   2. `allowedNonOwningMethod` was a NAME LIST shared by every head of the
//      family, so a name a SIBLING head declared was admitted on every head.
//
// THE FIX. The call surface is now the stdlib's own PER-HEAD TABLE
// (`concreteCollectionSurfaces()` in src/sema/ExprTypecheck_Calls.cpp), so
// the head that DECLARES a name and the head that does not can never
// disagree about it — the property #7700 established for the §26.5 rule-1
// lookup. The diagnostic is the stdlib's own documented-surface sentence,
// and its surface list is GENERATED from the same table the gate reads
// (issue #8333's lesson: a hand-copied list drifts from the table it claims
// to quote).
//
// THE ROWS:
//
//   REJECT rows (the five shapes) — each must draw the documented-surface
//   diagnostic naming the head, the stdlib section and the head's surface.
//
//   MESSAGE rows — the diagnostic must name the §-section that DECLARES the
//   head, must carry the documented-surface sentence, and must quote a
//   surface list that is COMPLETE for the head it names (`SortedDictionary`:
//   `Add`, `Count` and `TryAdd` are declared by §4.17 — the predecessor's
//   hand-copied list omitted them, which is issue #8333).
//
//   CONTROL rows — every DECLARED member must keep compiling AND running:
//   `Array<T>`'s `IndexOf`/`LastIndexOf` (§4.4), `OwningArray<T>`'s
//   `IndexOf` (§4.5), `Dictionary<K,V>`'s §4.8 surface, `SortedDictionary`'s
//   §4.17 surface, the Stack/Queue/Set/LinkedList surfaces, `FrozenArray`'s
//   §4.19 surface, stdlib §5's LINQ operators over a stdlib head, the
//   universal members of language spec §8.1, and a USER class's own member
//   of the same name.
//
// Citations verified verbatim against docs/Bezel_Standard_Library.md:
//   - §4.19 `FrozenArray<T>`: `public static method From(owned buffer:
//     OwningBuffer<T>): owned FrozenArray<T>;`, `From(items: Array<T>)`,
//     `public readonly property Length: int;`, the indexer `[i: int]: T`,
//     `public method ToArray(): owned Array<T>;`, `destructor;` — and NO
//     mutator (`Add`/`Clear`/`Remove`/`Insert` are not declared).
//   - §4.17 `SortedDictionary<K, V>`: `Count`, the indexer `[key: K]: V`,
//     `Add`, `TryAdd`, `ContainsKey`, `Contains`, `TryGetValue`, `GetOrNull`,
//     `Remove`, `Clear`, `FirstKey`, `LastKey` — and NO `Insert`.
//   - §4.4 `Array<T>`: `public method IndexOf(item: T, startIndex: int := 0):
//     int;` and `public method LastIndexOf(item: T): int;`.
//   - §4.5 `OwningArray<T>`: `public method IndexOf(item: T): int;`.
//   - §4.8 `Dictionary<K, V>`: `Count`, `Keys`, `Values`, `Add`, `TryAdd`,
//     `ContainsKey`, `Contains`, `TryGetValue`, `GetOrNull`, `GetOrAdd`,
//     `Remove`, `Clear`.
//   - §5: `public extension IEnumerable<T>` — the LINQ operators apply to
//     every head that conforms to `IEnumerable<T>`.
//   - language spec §8.1: the universal `ToString`/`Equals`/`GetHashCode`/
//     `GetType` members.
//
// The REJECT rows fail on the pre-fix compiler (the five shapes compile
// there); the CONTROL rows pass on BOTH arms — the gate must not refuse a
// DECLARED member.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// True iff some reported ERROR diagnostic carries `needle`. The precise
// wording is the regression-pin (per the lane rules: assert diagnostic
// TEXT, not hasErrors() alone).
bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// The program must be REFUSED, and the refusal must carry `needle`.
void expectRejected(const std::string& src, const std::string& needle) {
    const JitResult r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, needle));
}

// The program must COMPILE and RUN, printing exactly `out`.
void expectOut(const std::string& src, const std::string& out) {
    const JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// A program with one `Array<int>` receiver holding 10, 20, 30 and one
// `Dictionary<int,int>` holding 1 -> 10.
std::string prog(const std::string& body) {
    return "module M; import System; import System.Collections.Generic;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    using var xs := new Array<int>();\n"
           "    xs.Add(10); xs.Add(20); xs.Add(30);\n"
           "    using var d := new Dictionary<int, int>();\n"
           "    d.Add(1, 100);\n" +
           body + "\n    return 0;\n  end Main;\nend C;\n";
}

}  // namespace

// ===========================================================================
// THE FIVE SHAPES — each is a member no stdlib section declares for the
// head, and each COMPILED before the fix.
// ===========================================================================

// stdlib §4.19 (v0.355): `FrozenArray<T>` has NO mutating member at all.
// `Add` is not declared for it.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           frozen_array_add_is_rejected) {
    expectRejected(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [1,2,1];\n"
        "    using var f := FrozenArray.From(a);\n"
        "    f.Add(1);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "no member `Add` on `FrozenArray`");
}

// stdlib §4.19 (v0.355): `Clear` is not declared for `FrozenArray<T>`.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           frozen_array_clear_is_rejected) {
    expectRejected(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [1,2,1];\n"
        "    using var f := FrozenArray.From(a);\n"
        "    f.Clear();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "no member `Clear` on `FrozenArray`");
}

// stdlib §4.19 (v0.355): `Remove` is not declared for `FrozenArray<T>`.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           frozen_array_remove_is_rejected) {
    expectRejected(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [1,2,1];\n"
        "    using var f := FrozenArray.From(a);\n"
        "    f.Remove(1);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "no member `Remove` on `FrozenArray`");
}

// stdlib §4.19 (v0.355): `Insert` is not declared for `FrozenArray<T>`.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           frozen_array_insert_is_rejected) {
    expectRejected(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [1,2,1];\n"
        "    using var f := FrozenArray.From(a);\n"
        "    f.Insert(0, 5);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "no member `Insert` on `FrozenArray`");
}

// stdlib §4.17 (v0.355): `SortedDictionary<K, V>` declares `Add`, `TryAdd`,
// `ContainsKey`, `Contains`, `TryGetValue`, `GetOrNull`, `Remove`, `Clear`,
// `Count`, `FirstKey`, `LastKey` and the indexer — it does NOT declare
// `Insert` (the map's write surface is the indexer / `Add`).
BEZEL_TEST(collection_undeclared_member_surface_8331,
           sorted_dictionary_insert_is_rejected) {
    expectRejected(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var sd := new SortedDictionary<int, int>();\n"
        "    sd.Insert(1, 10);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "no member `Insert` on `SortedDictionary`");
}

// ===========================================================================
// THE MESSAGE — the stdlib's own documented-surface sentence, naming the
// section that DECLARES the head.
// ===========================================================================

// stdlib §4.19 (v0.355): the refusal names the section that declares
// `FrozenArray<T>`'s surface and carries the documented-surface sentence.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           frozen_array_names_the_stdlib_section_and_surface) {
    const JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [1,2,1];\n"
        "    using var f := FrozenArray.From(a);\n"
        "    f.Add(1);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "(stdlib \xC2\xA7""4.19)"));
    BEZEL_EXPECT_TRUE(
        hasErrorContaining(r, "not part of this collection's documented surface"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "language spec \xC2\xA7""8.1"));
    BEZEL_EXPECT_TRUE(
        hasErrorContaining(r, "stdlib \xC2\xA7""5 LINQ extension operator"));
}

// stdlib §4.19 (v0.355): the quoted surface is the head's OWN — `Length`,
// `ToArray` and the `IEnumerable<T>`-inherited `GetEnumerator`.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           frozen_array_quotes_its_own_documented_surface) {
    const JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [1,2,1];\n"
        "    using var f := FrozenArray.From(a);\n"
        "    f.Add(1);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "GetEnumerator/Length/ToArray"));
}

// stdlib §4.17 (v0.355): the quoted surface is COMPLETE for the head it
// names — `Add`, `Count` and `TryAdd` are declared by §4.17 and must be in
// the list (issue #8333: the predecessor's hand-copied list omitted them).
BEZEL_TEST(collection_undeclared_member_surface_8331,
           sorted_dictionary_quotes_a_complete_documented_surface) {
    const JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var sd := new SortedDictionary<int, int>();\n"
        "    sd.Insert(1, 10);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "(stdlib \xC2\xA7""4.17)"));
    BEZEL_EXPECT_TRUE(
        hasErrorContaining(r, "Add/Clear/Contains/ContainsKey/Count/FirstKey/"
                              "GetEnumerator/GetOrNull/LastKey/Remove/TryAdd/"
                              "TryGetValue"));
}

// ===========================================================================
// CONTROLS — every DECLARED member keeps compiling AND running.
// ===========================================================================

// stdlib §4.4 (v0.355): `Array<T>` declares `IndexOf(item: T, startIndex:
// int := 0): int` and `LastIndexOf(item: T): int`.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           array_indexof_and_lastindexof_still_work) {
    expectOut(prog(
        "    Console.WriteLine(xs.IndexOf(20));\n"
        "    Console.WriteLine(xs.LastIndexOf(20));"),
              "1\n1\n");
}

// stdlib §4.5 (v0.355): `OwningArray<T>` declares `IndexOf(item: T): int`.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           owning_array_indexof_still_works) {
    expectOut(
        "module M; import System; import System.Collections.Generic;\n"
        "class Item\n"
        "  public V: int;\n"
        "  public constructor(v: int)\n"
        "    V := v;\n"
        "  end constructor;\n"
        "  destructor end destructor;\n"
        "end Item;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var oa := new OwningArray<Item>();\n"
        "    oa.Add(move (new Item(10)));\n"
        "    oa.Add(move (new Item(20)));\n"
        "    Console.WriteLine(oa.IndexOf(oa[1]));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "1\n");
}

// stdlib §4.8 (v0.355): `Dictionary<K, V>`'s declared surface — `Add`,
// `ContainsKey`, `Count`, `Keys`, `Values`, `TryGetValue`.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           dictionary_declared_surface_still_works) {
    expectOut(prog(
        "    Console.WriteLine(d.ContainsKey(1));\n"
        "    Console.WriteLine(d.Count);\n"
        "    Console.WriteLine(d.Keys.Count);\n"
        "    Console.WriteLine(d.Values.Count);\n"
        "    if d.TryGetValue(1, out var v) then Console.WriteLine(v); end if;"),
              "True\n1\n1\n1\n100\n");
}

// stdlib §4.17 (v0.355): `SortedDictionary<K, V>`'s declared surface —
// `Add`, `TryAdd`, `Count`, `ContainsKey`, `TryGetValue`, `Remove`, `Clear`,
// `FirstKey`, `LastKey`.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           sorted_dictionary_declared_surface_still_works) {
    expectOut(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var sd := new SortedDictionary<int, int>();\n"
        "    sd.Add(2, 20); sd.Add(1, 10);\n"
        "    Console.WriteLine(sd.TryAdd(3, 30));\n"
        "    Console.WriteLine(sd.Count);\n"
        "    Console.WriteLine(sd.ContainsKey(1));\n"
        "    if sd.TryGetValue(1, out var v) then Console.WriteLine(v); end if;\n"
        "    Console.WriteLine(sd.FirstKey);\n"
        "    Console.WriteLine(sd.LastKey);\n"
        "    Console.WriteLine(sd.Remove(1));\n"
        "    sd.Clear();\n"
        "    Console.WriteLine(sd.Count);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "True\n3\nTrue\n10\n1\n3\nTrue\n0\n");
}

// stdlib §4.11 / §4.10 / §4.6 (v0.355): the Stack, Queue, Set and
// LinkedList surfaces.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           stack_queue_set_linkedlist_surfaces_still_work) {
    expectOut(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var st := new Stack<int>();\n"
        "    st.Push(1);\n"
        "    Console.WriteLine(st.Peek());\n"
        "    using var q := new Queue<int>();\n"
        "    q.Enqueue(2);\n"
        "    Console.WriteLine(q.Dequeue());\n"
        "    using var set := new Set<int>();\n"
        "    set.Add(3);\n"
        "    Console.WriteLine(set.Contains(3));\n"
        "    using var ll := new LinkedList<int>();\n"
        "    ll.AddLast(4);\n"
        "    Console.WriteLine(ll.Count);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "1\n2\nTrue\n1\n");
}

// stdlib §4.19 (v0.355): `FrozenArray<T>`'s OWN declared surface — `Length`,
// the indexer and `ToArray`.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           frozen_array_declared_surface_still_works) {
    expectOut(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [1,2,1];\n"
        "    using var f := FrozenArray.From(a);\n"
        "    Console.WriteLine(f.Length);\n"
        "    Console.WriteLine(f[1]);\n"
        "    using var back := f.ToArray();\n"
        "    Console.WriteLine(back.Count);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "3\n2\n3\n");
}

// stdlib §5 (v0.355): the `extension IEnumerable<T>` operators apply to a
// stdlib head — `Array<T>`'s and `Dictionary<K, V>`'s.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           linq_extension_operators_still_work) {
    expectOut(prog(
        "    Console.WriteLine(xs.Where(lambda(x: int): bool => x > 5).Count());\n"
        "    Console.WriteLine(d.Select(lambda(e: DictionaryEntry<int,int>): int"
        " => e.Value).Count());"),
              "3\n1\n");
}

// language spec §8.1 (v0.355): the universal members on a stdlib head.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           universal_members_still_work) {
    expectOut(prog(
        "    Console.WriteLine(xs.ToString());\n"
        "    Console.WriteLine(xs.Equals(xs));\n"
        "    Console.WriteLine(xs.GetHashCode());"),
              "System.Collections.Generic.Array<int>\nTrue\n1\n");
}

// The rule is about the STDLIB's heads: a USER class's own member of the
// same name is untouched.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           user_class_member_still_works) {
    expectOut(
        "module M; import System;\n"
        "class Box\n"
        "  public V: int;\n"
        "  public constructor(v: int)\n"
        "    V := v;\n"
        "  end constructor;\n"
        "  public method Add(x: int): int\n"
        "    V := V + x;\n"
        "    return V;\n"
        "  end Add;\n"
        "  public method Clear(): int\n"
        "    V := 0;\n"
        "    return V;\n"
        "  end Clear;\n"
        "  public method Insert(x: int): int\n"
        "    return x;\n"
        "  end Insert;\n"
        "  public method Remove(x: int): int\n"
        "    return x;\n"
        "  end Remove;\n"
        "  destructor end destructor;\n"
        "end Box;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var b := new Box(1);\n"
        "    Console.WriteLine(b.Add(2));\n"
        "    Console.WriteLine(b.Clear());\n"
        "    Console.WriteLine(b.Insert(3));\n"
        "    Console.WriteLine(b.Remove(4));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "3\n0\n3\n4\n");
}

// ===========================================================================
// fix #8349 (stdlib §4.14, v0.355): the `OwningBuffer<T>` head gets its own
// row of the per-head table, so a call that names a member §4.14 does NOT
// declare draws the per-head documented-surface refusal instead of codegen's
// generic "produced no value, and no dispatch arm emitted any code" backstop
// (#2457).
// ===========================================================================

// stdlib §4.14 (v0.355) declares `OwningBuffer<T>`'s surface: the `Length`
// property, the indexer, `AsSpan` (four overloads), `AsReadOnlySpan` (four
// overloads) and — on the value-typed presentation — `DangerousPointer`.
// `BogusMember` is not among them, so the call is refused with the same
// per-head sentence every other head gets.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           owning_buffer_bogus_member_is_rejected) {
    expectRejected(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var buf := new OwningBuffer<int>();\n"
        "    buf.BogusMember();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "no member `BogusMember` on `OwningBuffer`");
}

// The refusal must name the stdlib section that DECLARES the head (§4.14)
// and quote the head's OWN surface — the list GENERATED from the row the
// gate reads (issue #8333's lesson: a hand-copied list drifts).
BEZEL_TEST(collection_undeclared_member_surface_8331,
           owning_buffer_refusal_quotes_the_section_4_14_surface) {
    expectRejected(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var buf := new OwningBuffer<int>();\n"
        "    buf.BogusMember();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "stdlib \xC2\xA7""4.14");
}

// CONTROL: §4.14's `Length` property keeps working, and the value it
// reports is the buffer's stored extent.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           owning_buffer_length_property_still_works) {
    expectOut(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var buf := new OwningBuffer<int>(3);\n"
        "    Console.WriteLine(buf.Length);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "3\n");
}

// CONTROL: the `Count` refusal keeps its OWN wording — "use `Length`" — and
// does not become the generic documented-surface sentence.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           owning_buffer_count_keeps_its_own_refusal) {
    expectRejected(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var buf := new OwningBuffer<int>();\n"
        "    buf.Count();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "no member `Count` on `OwningBuffer<T>`: use `Length` (stdlib "
        "\xC2\xA7""4.14)");
}

// CONTROL: `CompareTo` keeps its dedicated refusal — §4.14 declares no
// `IComparable<T>` conformance for the head — and is not shadowed by the
// generic documented-surface sentence.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           owning_buffer_compare_to_keeps_its_own_refusal) {
    expectRejected(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var buf := new OwningBuffer<int>();\n"
        "    buf.CompareTo(1);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "no member `CompareTo` on `OwningBuffer<T>` (stdlib \xC2\xA7""4.14 "
        "declares no `IComparable<T>` conformance for this type)");
}

// CONTROL: the conditional `DangerousPointer` keeps its own dedicated
// refusal (stdlib §4.14 documents it; this compiler does not implement it).
BEZEL_TEST(collection_undeclared_member_surface_8331,
           owning_buffer_dangerous_pointer_keeps_its_own_refusal) {
    expectRejected(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var buf := new OwningBuffer<int>();\n"
        "    buf.DangerousPointer();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "`DangerousPointer()` on `OwningBuffer<T>` is documented in stdlib "
        "\xC2\xA7""4.14");
}

// CONTROL: the members §4.14 DOES declare keep compiling and running — the
// two span views.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           owning_buffer_span_views_still_work) {
    expectOut(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var buf := new OwningBuffer<int>(2);\n"
        "    buf[0] := 5;\n"
        "    Console.WriteLine(buf.AsSpan().Length);\n"
        "    Console.WriteLine(buf.AsReadOnlySpan().Length);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "2\n2\n");
}

// CONTROL: the members the COMPILER dispatches for the head although stdlib
// §4.14 declares no such member (the table's fourth field) keep compiling
// AND keep working — `Get`, `Peek`, `Pop`, `Clear` and `Sort`. Without
// them in that field they would start drawing the surface diagnostic, which
// would be a regression: a member that compiles today must not stop
// compiling.
BEZEL_TEST(collection_undeclared_member_surface_8331,
           owning_buffer_compiler_dispatched_members_still_work) {
    expectOut(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var buf := new OwningBuffer<int>(3);\n"
        "    buf[0] := 7; buf[1] := 8; buf[2] := 9;\n"
        "    Console.WriteLine(buf.Get(0));\n"
        "    Console.WriteLine(buf.Peek());\n"
        "    buf.Pop();\n"
        "    Console.WriteLine(buf.Length);\n"
        "    buf.Clear();\n"
        "    Console.WriteLine(buf.Length);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "7\n9\n2\n0\n");
}

// fix #8349 (stdlib §4.14, v0.355): the ELEMENT-TAKING forms of the
// members the compiler dispatches for the head although stdlib §4.14
// declares no such member. The dispatch is ARITY-DEPENDENT for this head —
// the arity-0 `Add()` reaches codegen's "no dispatch arm" backstop, while
// the element-taking `Add(item)` is really dispatched (the buffer's
// array-backed storage answers it) — so the row that measures the surface
// has to probe the element-taking forms too.
//
// MEASURED on the base tree: `Add(11)` grows the buffer (3 → 4) and
// `IndexOf(42)` finds the slot (1).
BEZEL_TEST(collection_undeclared_member_surface_8331,
           owning_buffer_element_taking_dispatched_members_still_work) {
    expectOut(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var buf := new OwningBuffer<int>(3);\n"
        "    buf[0] := 7; buf[1] := 42; buf[2] := 9;\n"
        "    buf.Add(11);\n"
        "    Console.WriteLine(buf.Length);\n"
        "    Console.WriteLine(buf.IndexOf(42));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "4\n1\n");
}
