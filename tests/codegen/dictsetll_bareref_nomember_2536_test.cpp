// Sema (via the AOT compile path) — issue #2536: a BARE reference to ANY
// bogus member on `OwningDictionary<K, V>` (or the other unhandled builtin
// collection families — `Dictionary`, `SortedDictionary`, `Set`,
// `OwningSet`, `SortedSet`, `LinkedList`, `OwningLinkedList`) ICE'd the
// compiler instead of producing a clean "no member" diagnostic. The CALL
// form `d.Bogus()` already rejected cleanly via the
// allowedOwningMethod/allowedNonOwningMethod lists in
// `ExprTypecheck_Calls.cpp` (~3975/~4067), but the BARE form
// `var f := d.Bogus;` fell through every `resolveMemberAccess` block
// untyped and codegen fell back to the #2434 "lowerLocalDecl's
// initializer failed to lower" internal (degraded to a zero-initialized
// value) — the ICE this test pins.
//
// Per stdlib §4.7/§4.8/§4.9/§4.10/§4.17/§4.18, these eight families have
// a closed documented surface: declared methods (Add/ContainsKey/Remove/
// etc. for Dictionary; Add/Remove/Contains/UnionWith/etc. for Set;
// AddFirst/AddLast/Find/etc. for LinkedList; + the OWNING-specific
// PopValue/TryPopValue/Drain for OwningDictionary; etc.), the universal
// Object members (ToString/Equals/GetHashCode/GetType/ReferenceEquals,
// language spec §8.1), the stdlib §5 LINQ extension operators
// (Where/Select/Take/etc.), and a small set of PROPERTIES (Count on
// every family, Keys/Values on Dictionary/OwningDictionary/
// SortedDictionary). A bare reference to a name outside that surface
// must reject with a diagnostic citing the family's stdlib section
// and its documented members — exactly the shape the CALL form already
// produces via the per-call-site lists.
//
// This test asserts on the diagnostic TEXT (per tests/README.md:
// runAot's WIFSIGNALED path synthesizes an Error diagnostic for a
// runtime crash too, so a genuine rejection must be distinguished by
// its message, not by exit code or `!r.ran` alone).

#include "support/aot_support.hpp"   // runAot — AOT compile path
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an
// Error diagnostic for a runtime crash, so a genuine compile-time
// rejection must be distinguished by asserting on the diagnostic TEXT,
// never by exit code or `!r.ran` alone.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

bool hasInternalIce(const std::string& program) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find("lowerLocalDecl's initializer") !=
                    std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// ── The issue's exact repro: OwningDictionary bare-reference to a bogus
//    member must reject with the same "no member" shape the call form
//    already produces (stdlib §4.9, mirroring the #2457 per-family blocks
//    for Array/Stack/Queue). ─────────────────────────────────────────────

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           owningdictionary_bogus_bare_ref_rejected_spec_4_9) {
    const std::string program =
        "module M; "
        "class V public N: int := 0; "
        "public constructor(n: int) this.N := n; end constructor; end V; "
        "class C public static method Main(): int "
        "using var d := new OwningDictionary<string, V>(); "
        "var f := d.TotallyBogusMember; "  // bare reference — not a member
        "return 0; "
        "end Main; end C;";
    // The §4.9 "no member" diagnostic must fire...
    BEZEL_EXPECT_TRUE(hasError(program, "no member `TotallyBogusMember`"));
    BEZEL_EXPECT_TRUE(hasError(program, "OwningDictionary"));
    BEZEL_EXPECT_TRUE(hasError(program, "\xC2\xA7""4.9"));
    // ...and the pre-fix #2434 ICE ("lowerLocalDecl's initializer failed
    // to lower") must NOT — that is the defect this fix replaces with a
    // clean reject.
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

// ── The CALL form `d.Bogus()` must continue to reject cleanly (via the
//    pre-existing allowedOwningMethod list at ~3975). Verifies no
//    double-report and no regression to silent acceptance. ─────────────

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           owningdictionary_bogus_call_form_still_rejected) {
    const std::string program =
        "module M; "
        "class V public N: int := 0; "
        "public constructor(n: int) this.N := n; end constructor; end V; "
        "class C public static method Main(): int "
        "using var d := new OwningDictionary<string, V>(); "
        "d.TotallyBogusMethod(1, 2); "
        "d.Dispose(); "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "no member `TotallyBogusMethod`"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

// ── False-reject guard: legitimate CALL forms on OwningDictionary
//    (Add/ContainsKey/Remove/Clear) must continue to compile. A bare
//    reference to a REAL documented member is deliberately left to fall
//    through to the pre-existing #2470-shape behavior (a separate
//    "real member but no function value" fix) — this test only pins
//    the CALL form, which must keep working. ────────────────────────────

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           owningdictionary_legitimate_call_forms_unaffected) {
    const std::string program =
        "module M; "
        "class V public N: int := 0; "
        "public constructor(n: int) this.N := n; end constructor; end V; "
        "class C public static method Main(): int "
        "using var d := new OwningDictionary<string, V>(); "
        "d.Add(\"a\", new V(1)); "
        "var b1: bool := d.ContainsKey(\"a\"); "
        "var b2: bool := d.Contains(\"a\"); "
        "var b3: bool := d.Remove(\"a\"); "
        "d.Clear(); "
        "d.Dispose(); "
        "return 0; "
        "end Main; end C;";
    // No "no member" diagnostic for any of the documented methods.
    BEZEL_EXPECT_TRUE(
        !hasError(program, "no member `Add`"));
    BEZEL_EXPECT_TRUE(
        !hasError(program, "no member `ContainsKey`"));
    BEZEL_EXPECT_TRUE(
        !hasError(program, "no member `Remove`"));
    BEZEL_EXPECT_TRUE(
        !hasError(program, "no member `Clear`"));
}

// ── Sweep the shape: the same "no member" reject must also cover the
//    other unhandled builtin collection families ─────────────────────────

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           dictionary_bogus_bare_ref_rejected_spec_4_8) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var d := new Dictionary<string, int>(); "
        "var f := d.TotallyBogusMember; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "no member `TotallyBogusMember`"));
    BEZEL_EXPECT_TRUE(hasError(program, "Dictionary"));
    BEZEL_EXPECT_TRUE(hasError(program, "\xC2\xA7""4.8"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           sorteddictionary_bogus_bare_ref_rejected_spec_4_17) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var d := new SortedDictionary<string, int>(); "
        "var f := d.TotallyBogusMember; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "no member `TotallyBogusMember`"));
    BEZEL_EXPECT_TRUE(hasError(program, "SortedDictionary"));
    BEZEL_EXPECT_TRUE(hasError(program, "\xC2\xA7""4.17"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           set_bogus_bare_ref_rejected_spec_4_10) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var s := new Set<int>(); "
        "s.Add(1); "
        "var f := s.TotallyBogusSetMember; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "no member `TotallyBogusSetMember`"));
    BEZEL_EXPECT_TRUE(hasError(program, "Set"));
    BEZEL_EXPECT_TRUE(hasError(program, "\xC2\xA7""4.10"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           owningset_bogus_bare_ref_rejected_spec_4_10) {
    // OwningSet<T> requires T: class (§4.2 / §14.4) — use a tiny class
    // for the element type, not an int.
    const std::string program =
        "module M; "
        "class E public N: int := 0; end E; "
        "class C public static method Main(): int "
        "using var s := new OwningSet<E>(); "
        "s.Add(new E()); "
        "var f := s.TotallyBogusOSetMember; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program, "no member `TotallyBogusOSetMember`"));
    BEZEL_EXPECT_TRUE(hasError(program, "OwningSet"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           sortedset_bogus_bare_ref_rejected_spec_4_18) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var s := new SortedSet<int>(); "
        "s.Add(1); "
        "var f := s.TotallyBogusSSMember; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "no member `TotallyBogusSSMember`"));
    BEZEL_EXPECT_TRUE(hasError(program, "SortedSet"));
    BEZEL_EXPECT_TRUE(hasError(program, "\xC2\xA7""4.18"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           linkedlist_bogus_bare_ref_rejected_spec_4_6) {
    // #2539 (sibling of #2536): `LinkedList<T>` is stdlib §4.6 (borrowing);
    // `OwningLinkedList<T>` is stdlib §4.7 (owning). The pre-#2539 generic
    // §-citation in ExprTypecheck_Calls.cpp~12792 cited §4.7 for BOTH —
    // rename + assertion updated so this test now FAILS without the fix
    // (the diagnostic cites §4.7) and PASSES with it (§4.6).
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var ll := new LinkedList<int>(); "
        "ll.AddFirst(1); "
        "var f := ll.TotallyBogusLLMember; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "no member `TotallyBogusLLMember`"));
    BEZEL_EXPECT_TRUE(hasError(program, "LinkedList"));
    BEZEL_EXPECT_TRUE(hasError(program, "\xC2\xA7""4.6"));
    // #2539 also pins the NEGATIVE: the wrong (owning-family) heading must
    // NOT appear in the borrowing diagnostic.
    BEZEL_EXPECT_TRUE(
        !hasError(program, "\xC2\xA7""4.7"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           owninglinkedlist_bogus_bare_ref_rejected_spec_4_7) {
    // #2539 (sibling of #2536): `OwningLinkedList<T>` is stdlib §4.7 (owning).
    // OwningLinkedList<T> requires T: class (§4.2 / §14.4).
    const std::string program =
        "module M; "
        "class E public N: int := 0; end E; "
        "class C public static method Main(): int "
        "using var ll := new OwningLinkedList<E>(); "
        "ll.AddFirst(new E()); "
        "var f := ll.TotallyBogusOLLMember; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program, "no member `TotallyBogusOLLMember`"));
    BEZEL_EXPECT_TRUE(hasError(program, "OwningLinkedList"));
    BEZEL_EXPECT_TRUE(hasError(program, "\xC2\xA7""4.7"));
    // #2539 pins the NEGATIVE for the owning direction: the wrong
    // (borrowing-family) heading §4.6 must NOT appear in the diagnostic.
    BEZEL_EXPECT_TRUE(
        !hasError(program, "\xC2\xA7""4.6"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

// ── False-reject guard: Count (a property) on SortedSet/SortedDictionary
//    must NOT be rejected as "no member" — the property handler at ~10947
//    explicitly omits SortedSet/SortedDictionary from its Count-typing
//    list (a pre-existing gap), so a bare reference to `s.Count` would
//    otherwise fall through to this block and be wrongly rejected. ──────

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           sortedset_count_not_false_rejected) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var s := new SortedSet<int>(); "
        "s.Add(1); "
        "var c: int := s.Count; "
        "return c; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(!hasError(program, "no member `Count`"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           sorteddictionary_count_not_false_rejected) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var d := new SortedDictionary<string, int>(); "
        "d[\"a\"] := 1; "
        "var c: int := d.Count; "
        "return c; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(!hasError(program, "no member `Count`"));
}

// ── False-reject guard: the #2521 OwningDictionary GetEnumerator case
//    must keep its DISTINCT diagnostic — the §4.9 "implements
//    IDisposable only" wording — not the generic "no member" wording
//    from this fix. The #2521 block runs first and returns, so the
//    generic block here must never fire for that case. ─────────────────

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           owningdictionary_getenumerator_distinct_message_preserved) {
    const std::string program =
        "module M; "
        "class V public N: int := 0; "
        "public constructor(n: int) this.N := n; end constructor; end V; "
        "class C public static method Main(): int "
        "using var d := new OwningDictionary<string, V>(); "
        "d[\"a\"] := new V(1); "
        "var f := d.GetEnumerator; "
        "return 0; "
        "end Main; end C;";
    // The #2521 distinct diagnostic must still fire...
    BEZEL_EXPECT_TRUE(
        hasError(program, "implements `IDisposable` only"));
    // ...and the generic "no member" diagnostic from THIS block must NOT.
    BEZEL_EXPECT_TRUE(
        !hasError(program, "it is not part of this collection's "
                           "documented surface"));
}

// ── Issue #2538 (follow-up to #2536): the #2536 catch-all's allow-list of
//    documented members was incomplete — the documented members inherited
//    from the underlying interfaces (IEnumerable<T> via ICollection<T> /
//    IReadOnlyCollection<T> / IOwningCollection<T>, ICollection<T>/
//    IOwningCollection<T>'s IsReadOnly, and the Set/OwningSet/SortedSet
//    TryAdd) were missing. A bare reference to a real documented member is
//    supposed to fall through to the #2537 "real member but no function
//    value" path (the symptom this catch-all is designed to avoid
//    conflating with the bogus-name case); the omission converted that
//    fall-through into a false "no member" reject. The fix expands the
//    allow-list (isDeclaredMethod / isKnownProperty) inside the #2536 block
//    and the family-specific surface citation strings.
//    Per stdlib §4.1/§4.10/§4.12/§4.18: GetEnumerator is on every collection
//    that inherits IEnumerable<T> (Dictionary/SortedDictionary via
//    ICollection<DictionaryEntry<K,V>>/IReadOnlyCollection<DictionaryEntry
//    <K,V>>, Set via ICollection<T>, OwningSet/OwningLinkedList via
//    IOwningCollection<T> → IReadOnlyCollection<T>, SortedSet via
//    IReadOnlyCollection<T>, LinkedList via ICollection<T>). OwningDictionary
//    correctly omits per §4.9 (the #2521 distinct diagnostic handles that).
//    Per stdlib §4.10/§4.18: TryAdd is documented on Set/OwningSet/SortedSet.
//    Per stdlib §4.1/§4.2: IsReadOnly is documented on ICollection<T> and
//    IOwningCollection<T> (every family that conforms to either).
//    Per stdlib §4.7: PopFirst/PopLast/TryPopFirst/TryPopLast are documented
//    on OwningLinkedList only.
//    Each axis below asserts the bare-reference does NOT produce the
//    generic "no member" diagnostic from THIS block. The bare-reference
//    itself is expected to fall through to the #2537 / #2434 path (filed
//    separately) — what we are pinning here is only the false-reject. ───

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           dictionary_getenumerator_not_false_rejected) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var d := new Dictionary<string, int>(); "
        "d[\"a\"] := 1; "
        "var f := d.GetEnumerator; "
        "return 0; "
        "end Main; end C;";
    // The generic "no member" reject from the #2536 block must NOT fire.
    BEZEL_EXPECT_TRUE(
        !hasError(program, "no member `GetEnumerator` on `Dictionary`"));
    // The surface citation must mention GetEnumerator now (so the
    // diagnostic is internally consistent if a future name DOES reject).
    // Verified indirectly: if it produced the reject, the §4.8 citation
    // WOULD include GetEnumerator — the test above is the load-bearing
    // assertion.
    BEZEL_EXPECT_TRUE(
        !hasError(program, "GetEnumerator` on `Dictionary` (stdlib "
                           "\xC2\xA7""4.8)"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           sorteddictionary_getenumerator_not_false_rejected) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var d := new SortedDictionary<string, int>(); "
        "d[\"a\"] := 1; "
        "var f := d.GetEnumerator; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        !hasError(program, "GetEnumerator` on `SortedDictionary`"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           set_getenumerator_not_false_rejected) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var s := new Set<int>(); "
        "s.Add(1); "
        "var f := s.GetEnumerator; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        !hasError(program, "GetEnumerator` on `Set`"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           set_tryadd_not_false_rejected) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var s := new Set<int>(); "
        "var f := s.TryAdd; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        !hasError(program, "TryAdd` on `Set`"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           owningset_tryadd_not_false_rejected) {
    // OwningSet<T> requires T: class (§4.2 / §14.4) — use a tiny class
    // for the element type, not an int.
    const std::string program =
        "module M; "
        "class E public N: int := 0; end E; "
        "class C public static method Main(): int "
        "using var s := new OwningSet<E>(); "
        "var f := s.TryAdd; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        !hasError(program, "TryAdd` on `OwningSet`"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           sortedset_getenumerator_not_false_rejected) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var s := new SortedSet<int>(); "
        "s.Add(1); "
        "var f := s.GetEnumerator; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        !hasError(program, "GetEnumerator` on `SortedSet`"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           sortedset_tryadd_not_false_rejected) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var s := new SortedSet<int>(); "
        "var f := s.TryAdd; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        !hasError(program, "TryAdd` on `SortedSet`"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           linkedlist_getenumerator_not_false_rejected) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var ll := new LinkedList<int>(); "
        "ll.AddFirst(1); "
        "var f := ll.GetEnumerator; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        !hasError(program, "GetEnumerator` on `LinkedList`"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           owninglinkedlist_getenumerator_not_false_rejected) {
    const std::string program =
        "module M; "
        "class E public N: int := 0; end E; "
        "class C public static method Main(): int "
        "using var ll := new OwningLinkedList<E>(); "
        "ll.AddFirst(new E()); "
        "var f := ll.GetEnumerator; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        !hasError(program, "GetEnumerator` on `OwningLinkedList`"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           owninglinkedlist_popfirst_not_false_rejected) {
    const std::string program =
        "module M; "
        "class E public N: int := 0; end E; "
        "class C public static method Main(): int "
        "using var ll := new OwningLinkedList<E>(); "
        "ll.AddFirst(new E()); "
        "var f := ll.PopFirst; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        !hasError(program, "PopFirst` on `OwningLinkedList`"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           owninglinkedlist_trypopfirst_not_false_rejected) {
    const std::string program =
        "module M; "
        "class E public N: int := 0; end E; "
        "class C public static method Main(): int "
        "using var ll := new OwningLinkedList<E>(); "
        "ll.AddFirst(new E()); "
        "var f := ll.TryPopFirst; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        !hasError(program, "TryPopFirst` on `OwningLinkedList`"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           dictionary_isreadonly_not_false_rejected) {
    // Per stdlib §4.1: IsReadOnly is the documented property on
    // ICollection<T> (every standard collection implementing it inherits
    // it; §4.1 prose names Array<T>/LinkedList<T>/Set<T>/Dictionary<K,V>).
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var d := new Dictionary<string, int>(); "
        "var f := d.IsReadOnly; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        !hasError(program, "IsReadOnly` on `Dictionary`"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           linkedlist_isreadonly_not_false_rejected) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var ll := new LinkedList<int>(); "
        "var f := ll.IsReadOnly; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        !hasError(program, "IsReadOnly` on `LinkedList`"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           owningset_isreadonly_not_false_rejected) {
    // Per stdlib §4.2: IsReadOnly is documented on IOwningCollection<T>.
    const std::string program =
        "module M; "
        "class E public N: int := 0; end E; "
        "class C public static method Main(): int "
        "using var s := new OwningSet<E>(); "
        "var f := s.IsReadOnly; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        !hasError(program, "IsReadOnly` on `OwningSet`"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           owninglinkedlist_isreadonly_not_false_rejected) {
    const std::string program =
        "module M; "
        "class E public N: int := 0; end E; "
        "class C public static method Main(): int "
        "using var ll := new OwningLinkedList<E>(); "
        "var f := ll.IsReadOnly; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        !hasError(program, "IsReadOnly` on `OwningLinkedList`"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           owningdictionary_isreadonly_still_rejected) {
    // OwningDictionary implements `IDisposable` only (§4.9) — it does NOT
    // implement IOwningCollection<T> or any interface that declares
    // IsReadOnly, so IsReadOnly must CONTINUE to reject. (The #2536
    // block's allow-list deliberately omits it.)
    const std::string program =
        "module M; "
        "class V public N: int := 0; "
        "public constructor(n: int) this.N := n; end constructor; end V; "
        "class C public static method Main(): int "
        "using var d := new OwningDictionary<string, V>(); "
        "var f := d.IsReadOnly; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program, "no member `IsReadOnly`"));
    BEZEL_EXPECT_TRUE(
        !hasError(program, "lowerLocalDecl's initializer"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           owningdictionary_getenumerator_still_rejected) {
    // OwningDictionary's GetEnumerator rejection must remain the #2521
    // DISTINCT diagnostic ("implements `IDisposable` only"), not the
    // generic "no member" from THIS block. The #2521 block at ~12560 runs
    // first and returns before the #2536 generic catch-all at ~12628.
    const std::string program =
        "module M; "
        "class V public N: int := 0; "
        "public constructor(n: int) this.N := n; end constructor; end V; "
        "class C public static method Main(): int "
        "using var d := new OwningDictionary<string, V>(); "
        "var f := d.GetEnumerator; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program, "implements `IDisposable` only"));
    BEZEL_EXPECT_TRUE(
        !hasError(program, "it is not part of this collection's "
                           "documented surface"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           bogus_bare_ref_still_rejected_after_fix) {
    // Regression guard: the fix expands the allow-list, but the
    // out-of-surface rejection must still fire for genuinely bogus names.
    // Picks one per family to confirm the diagnostic still cites the
    // family's stdlib section.
    const std::string dictProg =
        "module M; "
        "class C public static method Main(): int "
        "var d := new Dictionary<string, int>(); "
        "var f := d.TotallyBogus; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(dictProg, "no member `TotallyBogus` on `Dictionary`"));
    BEZEL_EXPECT_TRUE(
        hasError(dictProg, "\xC2\xA7""4.8"));

    const std::string setProg =
        "module M; "
        "class C public static method Main(): int "
        "var s := new Set<int>(); "
        "var f := s.TotallyBogus; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(setProg, "no member `TotallyBogus` on `Set`"));
    BEZEL_EXPECT_TRUE(
        hasError(setProg, "\xC2\xA7""4.10"));

    const std::string osProg =
        "module M; "
        "class E public N: int := 0; end E; "
        "class C public static method Main(): int "
        "using var s := new OwningSet<E>(); "
        "var f := s.TotallyBogus; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(osProg, "no member `TotallyBogus` on `OwningSet`"));

    const std::string ollProg =
        "module M; "
        "class E public N: int := 0; end E; "
        "class C public static method Main(): int "
        "using var ll := new OwningLinkedList<E>(); "
        "var f := ll.TotallyBogus; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(ollProg, "no member `TotallyBogus` on `OwningLinkedList`"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           sortedset_bogus_surface_cites_tryadd) {
    // The fix updates the SortedSet surface citation to include TryAdd.
    // A genuinely bogus name on SortedSet must reject, and the diagnostic
    // for an unrelated "Add" (already in the list) — wait, Add IS in the
    // list, so that's not a good choice. Use a name that's NEVER been
    // documented on SortedSet, e.g. "Tap" — should reject and the
    // surface citation mentions TryAdd.
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var s := new SortedSet<int>(); "
        "s.Add(1); "
        "var f := s.Tap; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program, "no member `Tap` on `SortedSet`"));
    // The surface citation should mention TryAdd (now in the per-family
    // surface string for SortedSet).
    BEZEL_EXPECT_TRUE(
        hasError(program, "TryAdd"));
    // And it should mention §4.18.
    BEZEL_EXPECT_TRUE(
        hasError(program, "\xC2\xA7""4.18"));
}

// ── Issue #2537 (sibling of #2536; mirror of #2470/#2492; language spec
//    §27.6): a BARE reference to a REAL, documented METHOD on the same
//    eight families the #2536 catch-all covers — OwningDictionary,
//    Dictionary, SortedDictionary, Set, OwningSet, SortedSet, LinkedList,
//    OwningLinkedList — used to ICE (the #2434 silent zero-init
//    degradation: codegen's `lowerLocalDecl` fallback emitted an "internal:
//    lowerLocalDecl's initializer expression failed to lower" message
//    because `ma`'s exprType was left UNSET and the LocalDecl's inferred
//    type was lost). The #2536 block's allow-list for documented members
//    (`isDeclaredMethod || isLinqOperator || isUniversalObjectMember`)
//    deliberately EXCLUDED them from the generic "no member" reject, so
//    the bare reference fell through. The fix adds a SECOND reject path
//    immediately before the #2536 catch-all that fires the same §27.6
//    "real member but no function value" diagnostic #2470/#2492 used for
//    Array/Stack/Queue, citing the family's stdlib section and #2537.
//    The CALL form (exempted by `!isCallForm`) is completely unaffected.
//
//    Each axis below asserts BOTH the §27.6 reject fires AND the pre-fix
//    #2434 internal ICE does NOT (so the test FAILS without the fix: the
//    program compiled and silently ICE'd, no real reject fires; and
//    PASSES with the fix: the reject fires and the ICE is gone). Per
//    tests/README.md, asserting on the diagnostic TEXT is load-bearing —
//    runAot's WIFSIGNALED path synthesizes an Error diagnostic for a
//    runtime crash too, so a genuine rejection must be distinguished by
//    its message, not by exit code or `!r.ran` alone.

// ── Per-family documented-METHOD bare references (#2537) ────────────────

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           dictionary_add_bare_ref_realmember_rejected_2537) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var d := new Dictionary<string, int>(); "
        "d[\"a\"] := 1; "
        "var f := d.Add; "
        "f(\"b\", 2); "
        "return 0; "
        "end Main; end C;";
    // The §27.6 "real member but no function value" diagnostic must fire...
    BEZEL_EXPECT_TRUE(hasError(
        program, "a bare reference to `Add`"));
    BEZEL_EXPECT_TRUE(hasError(
        program, "is not yet supported on `Dictionary`"));
    BEZEL_EXPECT_TRUE(hasError(program, "\xC2\xA7""4.8"));
    BEZEL_EXPECT_TRUE(hasError(program, "#2537"));
    // ...and the pre-fix #2434 internal ICE must NOT.
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
    // ...and the generic "no member" wording from the #2536 catch-all
    // must NOT — the message must be the §27.6 one, not the "no member"
    // one (the member IS real).
    BEZEL_EXPECT_TRUE(
        !hasError(program, "no member `Add` on `Dictionary`"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           owningdictionary_popvalue_bare_ref_realmember_rejected_2537) {
    // Per stdlib §4.9: PopValue is OwningDictionary-specific (no
    // borrowing counterpart). Test the owning-only method to confirm the
    // #2537 reject covers it.
    const std::string program =
        "module M; "
        "class V public N: int := 0; "
        "public constructor(n: int) this.N := n; end constructor; end V; "
        "class C public static method Main(): int "
        "using var d := new OwningDictionary<string, V>(); "
        "d.Add(\"a\", new V(1)); "
        "var f := d.PopValue; "
        "d.Dispose(); "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "a bare reference to `PopValue`"));
    BEZEL_EXPECT_TRUE(hasError(
        program, "is not yet supported on `OwningDictionary`"));
    BEZEL_EXPECT_TRUE(hasError(program, "\xC2\xA7""4.9"));
    BEZEL_EXPECT_TRUE(hasError(program, "#2537"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
    BEZEL_EXPECT_TRUE(
        !hasError(program, "no member `PopValue` on `OwningDictionary`"));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           sorteddictionary_containskey_bare_ref_realmember_rejected_2537) {
    // Per stdlib §4.17: SortedDictionary drops Add/GetOrAdd but keeps
    // ContainsKey/TryGetValue/GetOrNull/Remove/Clear. Use ContainsKey.
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var d := new SortedDictionary<string, int>(); "
        "d[\"a\"] := 1; "
        "var f := d.ContainsKey; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "a bare reference to `ContainsKey`"));
    BEZEL_EXPECT_TRUE(hasError(
        program, "is not yet supported on `SortedDictionary`"));
    BEZEL_EXPECT_TRUE(hasError(program, "\xC2\xA7""4.17"));
    BEZEL_EXPECT_TRUE(hasError(program, "#2537"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           set_contains_bare_ref_realmember_rejected_2537) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var s := new Set<int>(); "
        "s.Add(1); "
        "var f := s.Contains; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "a bare reference to `Contains`"));
    BEZEL_EXPECT_TRUE(hasError(
        program, "is not yet supported on `Set`"));
    BEZEL_EXPECT_TRUE(hasError(program, "\xC2\xA7""4.10"));
    BEZEL_EXPECT_TRUE(hasError(program, "#2537"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           owningset_take_bare_ref_realmember_rejected_2537) {
    // Per stdlib §4.10: Take is OwningSet-specific. Confirm the #2537
    // reject covers owning-only methods on OwningSet.
    const std::string program =
        "module M; "
        "class E public N: int := 0; end E; "
        "class C public static method Main(): int "
        "using var s := new OwningSet<E>(); "
        "s.Add(new E()); "
        "var f := s.Take; "
        "s.Dispose(); "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "a bare reference to `Take`"));
    BEZEL_EXPECT_TRUE(hasError(
        program, "is not yet supported on `OwningSet`"));
    BEZEL_EXPECT_TRUE(hasError(program, "\xC2\xA7""4.10"));
    BEZEL_EXPECT_TRUE(hasError(program, "#2537"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           sortedset_remove_bare_ref_realmember_rejected_2537) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var s := new SortedSet<int>(); "
        "s.Add(1); "
        "var f := s.Remove; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "a bare reference to `Remove`"));
    BEZEL_EXPECT_TRUE(hasError(
        program, "is not yet supported on `SortedSet`"));
    BEZEL_EXPECT_TRUE(hasError(program, "\xC2\xA7""4.18"));
    BEZEL_EXPECT_TRUE(hasError(program, "#2537"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           linkedlist_addlast_bare_ref_realmember_rejected_2537) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var ll := new LinkedList<int>(); "
        "var f := ll.AddLast; "
        "f(1); "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "a bare reference to `AddLast`"));
    BEZEL_EXPECT_TRUE(hasError(
        program, "is not yet supported on `LinkedList`"));
    BEZEL_EXPECT_TRUE(hasError(program, "\xC2\xA7""4.6"));
    BEZEL_EXPECT_TRUE(hasError(program, "#2537"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           owninglinkedlist_popfirst_bare_ref_realmember_rejected_2537) {
    // Per stdlib §4.7: PopFirst/PopLast/TryPopFirst/TryPopLast are
    // OwningLinkedList-only. Confirm the #2537 reject covers them.
    const std::string program =
        "module M; "
        "class E public N: int := 0; end E; "
        "class C public static method Main(): int "
        "using var ll := new OwningLinkedList<E>(); "
        "ll.AddFirst(new E()); "
        "var f := ll.PopFirst; "
        "ll.Dispose(); "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "a bare reference to `PopFirst`"));
    BEZEL_EXPECT_TRUE(hasError(
        program, "is not yet supported on `OwningLinkedList`"));
    BEZEL_EXPECT_TRUE(hasError(program, "\xC2\xA7""4.7"));
    BEZEL_EXPECT_TRUE(hasError(program, "#2537"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

// ── Universal Object member (ToString) bare reference — same §27.6
//    reject applies; mirrors #2470's fold of isUniversalObjectMember
//    into the same path. ────────────────────────────────────────────────

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           set_tostring_bare_ref_realmember_rejected_2537) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var s := new Set<int>(); "
        "s.Add(1); "
        "var f := s.ToString; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "a bare reference to `ToString`"));
    BEZEL_EXPECT_TRUE(hasError(
        program, "is not yet supported on `Set`"));
    BEZEL_EXPECT_TRUE(hasError(program, "#2537"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

// ── LINQ operator (Where) bare reference — same §27.6 reject applies;
//    mirrors #2492's fold of isLinqOperator into the same path. ────────

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           dictionary_where_bare_ref_realmember_rejected_2537) {
    const std::string program =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "var d := new Dictionary<string, int>(); "
        "d[\"a\"] := 1; "
        "var f := d.Where; "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "a bare reference to `Where`"));
    BEZEL_EXPECT_TRUE(hasError(
        program, "is not yet supported on `Dictionary`"));
    BEZEL_EXPECT_TRUE(hasError(program, "#2537"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

// ── False-reject guard: the CALL form on a real method must still
//    compile and work. The §27.6 reject is gated on `!isCallForm`. ────

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           dictionary_add_call_form_unaffected_2537) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var d := new Dictionary<string, int>(); "
        "d[\"a\"] := 1; "
        "d.Add(\"b\", 2); "
        "var b: bool := d.ContainsKey(\"a\"); "
        "return (if b then 1 else 0); "
        "end Main; end C;";
    // No §27.6 reject, no "no member" reject, no ICE — the CALL form is
    // structurally dispatched in codegen (the #2537 reject is gated on
    // `!isCallForm`).
    BEZEL_EXPECT_TRUE(
        !hasError(program, "a bare reference to `Add`"));
    BEZEL_EXPECT_TRUE(
        !hasError(program, "no member `Add` on `Dictionary`"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           set_add_call_form_unaffected_2537) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var s := new Set<int>(); "
        "s.Add(1); "
        "s.Add(2); "
        "var b: bool := s.Contains(1); "
        "return (if b then 1 else 0); "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        !hasError(program, "a bare reference to `Add`"));
    BEZEL_EXPECT_TRUE(
        !hasError(program, "no member `Add` on `Set`"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           linkedlist_addfirst_call_form_unaffected_2537) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var ll := new LinkedList<int>(); "
        "ll.AddFirst(1); "
        "ll.AddFirst(2); "
        "return ll.Count; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        !hasError(program, "a bare reference to `AddFirst`"));
    BEZEL_EXPECT_TRUE(
        !hasError(program, "no member `AddFirst` on `LinkedList`"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}
