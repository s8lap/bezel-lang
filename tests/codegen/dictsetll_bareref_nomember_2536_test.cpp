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
           linkedlist_bogus_bare_ref_rejected_spec_4_7) {
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
    BEZEL_EXPECT_TRUE(hasError(program, "\xC2\xA7""4.7"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

BEZEL_TEST(dictsetll_bareref_nomember_2536,
           owninglinkedlist_bogus_bare_ref_rejected_spec_4_7) {
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
