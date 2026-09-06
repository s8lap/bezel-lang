// Regression tests for issue #3896 — the eight collection types #3883
// (OwningBuffer, "slice 3" of the closed-#2457 sweep) deliberately left for
// a follow-up slice: `Set`, `OwningSet`, `SortedSet`, `FrozenArray`,
// `Dictionary`, `OwningDictionary`, `LinkedList`, `OwningLinkedList`.
//
// Per stdlib §4.6 (LinkedList), §4.7 (OwningLinkedList), §4.8 (Dictionary),
// §4.9 (OwningDictionary), §4.10 (Set/OwningSet), §4.18 (SortedSet), §4.19
// (FrozenArray) — verified against the actual class listing for each,
// before writing this fix, not assumed — every one of the eight is a real,
// declaration-less `public class` with no Equals/GetHashCode/ToString
// override and no `IComparable<T>` conformance. So the same language spec
// §8.3 "Class" default (reference equality / identity-based hash) and §8.5
// default (qualified name) that #3883 implemented for `OwningBuffer<T>`
// apply mechanically here too, with the SAME two `isAuditedElsewhere`
// carve-outs (#2465's all-or-nothing gate) #3883 needed.
//
// #3896's own filed caveat is that these eight do NOT share one backing the
// way #2457's original six (Array/OwningArray/Stack/OwningStack/Queue/
// OwningQueue) and OwningBuffer do: Dictionary/OwningDictionary are
// `bz_dict_t*`-backed, LinkedList/OwningLinkedList are `bz_llist_t*`-backed,
// and Set/OwningSet are EITHER `bz_array_t*`-backed (a class/interface
// element — LlvmBackend_Setup.cpp's materializeArrayBackedSetLiteral) or
// `bz_dict_t*`-backed (a value/primitive/string element —
// materializeHeapSetLiteral) depending on the instantiation. This turns out
// NOT to matter for either default implemented here: both codegen sites
// (LlvmBackend_Calls2.cpp) treat the receiver purely as an opaque heap
// POINTER — `ICmpEQ` for Equals, `bz_obj_identity_hash` (pointer-to-int)
// for GetHashCode, a compile-time string literal for ToString — and never
// dereference a backing-specific field, so the same mechanism is correct
// regardless of representation. Confirmed per-type below with a real probe
// for each of the three backings (FrozenArray for bz_array_t*, Dictionary/
// OwningDictionary for bz_dict_t*, LinkedList/OwningLinkedList for
// bz_llist_t*, Set/OwningSet/SortedSet for the generic-key hash table).
//
// Per-type CompareTo carve-out note (ExprTypecheck_Calls.cpp's own comment
// records the same, measured, finding): SEVEN of the eight (all but
// FrozenArray) are ALSO named in the pre-existing #920/#1112
// isOwningContainer/isNonOwningContainer surface tables, whose method
// allowlists do not admit `CompareTo` either — so their CALL form already
// had an independent backstop before this fix (a second, "§22.5 / §4.x ...
// not part of this collection's method surface" diagnostic); this fix's own
// carve-out is genuinely load-bearing for ALL EIGHT in the BARE (uncalled)
// reference sub-case (`var f := x.CompareTo;`), which has no such backstop
// (the container checks only run inside a CallExpr), exactly like
// OwningBuffer. `FrozenArray` is absent from both container tables — like
// OwningBuffer — so this carve-out is load-bearing for its CALL form too.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <initializer_list>

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
bool hasErrorAll(const std::string& program,
                  std::initializer_list<const char*> keywords) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        bool all = true;
        for (const char* k : keywords) {
            if (d.message.find(k) == std::string::npos) { all = false; break; }
        }
        if (all) return true;
    }
    return false;
}

bool hasError(const std::string& program, const char* keyword) {
    return hasErrorAll(program, {keyword});
}

// No diagnostic anywhere cites the closed, stale #2457 issue — either the
// old #2465 placeholder's wording or the codegen "no dispatch arm" backstop's
// citation, both of which this fix's carve-outs preempt with precise text.
bool noStale2457Citation(const std::string& program) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("(#2457)") != std::string::npos ||
            d.message.find("declared via the universal Object contract") !=
                std::string::npos)
            return false;
    }
    return true;
}

// Builds a `module M; [helper] class C public static method Main(): int
// <body> end Main; end C;` program.
std::string program(const std::string& helper, const std::string& body) {
    return "module M; " + helper +
           "class C public static method Main(): int " + body +
           "end Main; end C;";
}

const char* kResClassHelper =
    "class Res public Tag: int; "
    "  public constructor(t: int) Tag := t; end constructor; "
    "end Res; ";

// Equals/GetHashCode identity-semantics body: two DISTINCT instances via
// `ctor1`/`ctor2`, self-equal, cross-unequal, hash stable across two reads.
std::string identityBody(const std::string& ctor1, const std::string& ctor2) {
    return
        "  var a1 := " + ctor1 + "; "
        "  var a2 := " + ctor2 + "; "
        "  var selfEq := a1.Equals(a1); "
        "  var crossEq := a1.Equals(a2); "
        "  var h1a := a1.GetHashCode(); "
        "  var h1b := a1.GetHashCode(); "
        "  if not selfEq then return 1; end; "
        "  if crossEq then return 2; end; "
        "  if h1a <> h1b then return 3; end; "
        "  return 42; ";
}

// Same as identityBody, but with an EXPLICIT type annotation on both
// locals. `FrozenArray<T>.From(...)`'s result TypeRef carries `declaration
// == builtinClassSym("FrozenArray")` with an EMPTY headName() (the
// "static-factory result shape" frozen_array_1723_test.cpp documents) —
// a DIFFERENT TypeRef shape from every other residual name's `new X()`
// receiver (`!declaration`, a real astNode head). This fix's sema/codegen
// changes are gated on the `!declaration` shape (matching #2457/#3883's own
// scope), so an UNANNOTATED `var a := FrozenArray<int>.From(...)` bypasses
// them entirely — confirmed live: `a.ToString()` compiles clean but returns
// the WRONG string with no diagnostic. This is a real, pre-existing gap
// (present before this fix touched anything — the declaration-set shape
// never reached the `!declaration`-gated #2465 placeholder either), out of
// scope for THIS fix and filed separately (see the commit body). An
// EXPLICIT type annotation (`var a: FrozenArray<T> := ...`) selects the
// OTHER shape — the declared type takes the `!declaration` bare-annotation
// form (mirroring the #1723 fix's own disambiguation) — which is the shape
// this fix actually audits, so these rows pin FrozenArray's fix on the
// shape it covers.
std::string identityBodyAnnotated(const std::string& typeAnnotation,
                                   const std::string& ctor1,
                                   const std::string& ctor2) {
    return
        "  var a1: " + typeAnnotation + " := " + ctor1 + "; "
        "  var a2: " + typeAnnotation + " := " + ctor2 + "; "
        "  var selfEq := a1.Equals(a1); "
        "  var crossEq := a1.Equals(a2); "
        "  var h1a := a1.GetHashCode(); "
        "  var h1b := a1.GetHashCode(); "
        "  if not selfEq then return 1; end; "
        "  if crossEq then return 2; end; "
        "  if h1a <> h1b then return 3; end; "
        "  return 42; ";
}

}  // namespace

// ═══════════════════════════ Set<T> (stdlib §4.10) ═══════════════════════
// Generic-key hash-table-backed for a value element (int here).

BEZEL_TEST(residual_collections_unresolved_member_3896,
           set_equals_and_gethashcode_identity_spec_8_3_stdlib_4_10) {
    BEZEL_EXPECT_EQ(
        run(program("", identityBody("new Set<int>()", "new Set<int>()"))),
        42);
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           set_tostring_implemented_literal_spec_8_5_stdlib_4_10) {
    const std::string p = program(
        "",
        "  var a := new Set<int>(); "
        "  var s := a.ToString(); "
        "  return (if s = \"System.Collections.Generic.Set<int>\" "
        "          then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           set_compareto_call_rejected_no_icomparable_stdlib_4_10) {
    const std::string p = program(
        "",
        "  var a1 := new Set<int>(); "
        "  var a2 := new Set<int>(); "
        "  var c := a1.CompareTo(a2); "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasError(p, "no member `CompareTo` on `Set<T>`"));
    BEZEL_EXPECT_TRUE(noStale2457Citation(p));
}

// #2536: a bare CompareTo reference is caught by the PRE-EXISTING,
// unconditional (not gated on `isAuditedElsewhere`) "not part of this
// collection's documented surface" catch-all for the seven
// isOwningContainer/isNonOwningContainer-tracked families — measured live
// (`./build/bezelc` probe) BEFORE assuming this fix's own `residualInfo`
// carve-out was reached: it is NOT, because #2536 already `return`s. This
// row pins that #2536's protection is unaffected by `Set` joining
// `isAuditedElsewhere`.
BEZEL_TEST(residual_collections_unresolved_member_3896,
           set_compareto_bare_reference_also_rejected_stdlib_4_10) {
    const std::string p = program(
        "",
        "  var a1 := new Set<int>(); "
        "  var f := a1.CompareTo; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"no member `CompareTo` on `Set`", "documented surface"}));
}

// #2537: a bare ToString/Equals/GetHashCode reference is caught by the
// PRE-EXISTING, unconditional #2537 "real member but no function value"
// reject for the same seven families — measured live, NOT this fix's own
// carve-out (which is unreachable here: #2537 already `return`s before this
// fix's later code runs). Confirms `Set` joining `isAuditedElsewhere` does
// not weaken this pre-existing protection.
BEZEL_TEST(residual_collections_unresolved_member_3896,
           set_bare_tostring_reference_rejected_not_degraded_stdlib_4_10) {
    const std::string p = program(
        "",
        "  var a := new Set<int>(); "
        "  var f := a.ToString; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"a bare reference to `ToString`", "not yet supported on `Set`",
            "#2537"}));
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           set_bare_equals_reference_rejected_not_degraded_stdlib_4_10) {
    const std::string p = program(
        "",
        "  var a := new Set<int>(); "
        "  var f := a.Equals; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"a bare reference to `Equals`", "not yet supported on `Set`",
            "#2537"}));
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           set_bare_gethashcode_reference_rejected_not_degraded_stdlib_4_10) {
    const std::string p = program(
        "",
        "  var a := new Set<int>(); "
        "  var f := a.GetHashCode; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"a bare reference to `GetHashCode`", "not yet supported on `Set`",
            "#2537"}));
}

// ═══════════════════════ OwningSet<T> (stdlib §4.10) ══════════════════════
// `where T: class` — a class element makes this the ARRAY-BACKED branch
// (materializeArrayBackedSetLiteral / bz_array_alloc), distinct from the
// borrowing `Set<int>` above (generic-key hash table). Both backings pin
// the same identity default.

BEZEL_TEST(residual_collections_unresolved_member_3896,
           owningset_equals_and_gethashcode_identity_spec_8_3_stdlib_4_10) {
    BEZEL_EXPECT_EQ(
        run(program(kResClassHelper,
                    identityBody("new OwningSet<Res>()",
                                 "new OwningSet<Res>()"))),
        42);
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           owningset_tostring_implemented_literal_spec_8_5_stdlib_4_10) {
    const std::string p = program(
        kResClassHelper,
        "  var a := new OwningSet<Res>(); "
        "  var s := a.ToString(); "
        "  return (if s = \"System.Collections.Generic.OwningSet<M.Res>\" "
        "          then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           owningset_compareto_call_rejected_no_icomparable_stdlib_4_10) {
    const std::string p = program(
        kResClassHelper,
        "  var a1 := new OwningSet<Res>(); "
        "  var a2 := new OwningSet<Res>(); "
        "  var c := a1.CompareTo(a2); "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasError(p, "no member `CompareTo` on `OwningSet<T>`"));
    BEZEL_EXPECT_TRUE(noStale2457Citation(p));
}

// #2536 (pre-existing, unaffected by `isAuditedElsewhere`) — see the `Set`
// row above for the full explanation; identical mechanism for OwningSet.
BEZEL_TEST(residual_collections_unresolved_member_3896,
           owningset_compareto_bare_reference_also_rejected_stdlib_4_10) {
    const std::string p = program(
        kResClassHelper,
        "  var a1 := new OwningSet<Res>(); "
        "  var f := a1.CompareTo; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"no member `CompareTo` on `OwningSet`", "documented surface"}));
}

// #2537 (pre-existing, unaffected by `isAuditedElsewhere`) — see the `Set`
// row above.
BEZEL_TEST(residual_collections_unresolved_member_3896,
           owningset_bare_tostring_reference_rejected_not_degraded_stdlib_4_10) {
    const std::string p = program(
        kResClassHelper,
        "  var a := new OwningSet<Res>(); "
        "  var f := a.ToString; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"a bare reference to `ToString`",
            "not yet supported on `OwningSet`", "#2537"}));
}

// ═══════════════════════ SortedSet<T> (stdlib §4.18) ══════════════════════
// Generic-key hash table (§4.18: "shares the generic-key hash table
// (bz_dict_alloc_generic)"), NOT a `bz_array_t*`-backed wrap source.

BEZEL_TEST(residual_collections_unresolved_member_3896,
           sortedset_equals_and_gethashcode_identity_spec_8_3_stdlib_4_18) {
    BEZEL_EXPECT_EQ(
        run(program("", identityBody("new SortedSet<int>()",
                                      "new SortedSet<int>()"))),
        42);
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           sortedset_tostring_implemented_literal_spec_8_5_stdlib_4_18) {
    const std::string p = program(
        "",
        "  var a := new SortedSet<int>(); "
        "  var s := a.ToString(); "
        "  return (if s = \"System.Collections.Generic.SortedSet<int>\" "
        "          then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           sortedset_compareto_call_rejected_no_icomparable_stdlib_4_18) {
    const std::string p = program(
        "",
        "  var a1 := new SortedSet<int>(); "
        "  var a2 := new SortedSet<int>(); "
        "  var c := a1.CompareTo(a2); "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasError(p, "no member `CompareTo` on `SortedSet<T>`"));
    BEZEL_EXPECT_TRUE(noStale2457Citation(p));
}

// #2536 (pre-existing, unaffected by `isAuditedElsewhere`) — see the `Set`
// row above.
BEZEL_TEST(residual_collections_unresolved_member_3896,
           sortedset_compareto_bare_reference_also_rejected_stdlib_4_18) {
    const std::string p = program(
        "",
        "  var a1 := new SortedSet<int>(); "
        "  var f := a1.CompareTo; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"no member `CompareTo` on `SortedSet`", "documented surface"}));
}

// #2537 (pre-existing, unaffected by `isAuditedElsewhere`) — see the `Set`
// row above.
BEZEL_TEST(residual_collections_unresolved_member_3896,
           sortedset_bare_equals_reference_rejected_not_degraded_stdlib_4_18) {
    const std::string p = program(
        "",
        "  var a := new SortedSet<int>(); "
        "  var f := a.Equals; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"a bare reference to `Equals`",
            "not yet supported on `SortedSet`", "#2537"}));
}

// ═══════════════════════ FrozenArray<T> (stdlib §4.19) ════════════════════
// `bz_array_t*`-backed, same representation as OwningBuffer, and — like
// OwningBuffer — absent from BOTH the #920/#1112 container tables and from
// the #2536/#2537 six-family bare-reference mechanism, so this fix's OWN
// carve-outs are load-bearing here (not merely diagnostic precision, unlike
// the other six residual names' CALL-form CompareTo case — see the commit
// body's triage table).
//
// EVERY local below uses an EXPLICIT type annotation
// (`var a: FrozenArray<int> := ...`) — see `identityBodyAnnotated`'s
// comment for why: the UNANNOTATED `var a := FrozenArray<int>.From(...)`
// idiom takes a DIFFERENT TypeRef shape (`declaration` set to a
// builtinClassSym, per frozen_array_1723_test.cpp) that this fix's
// `!declaration`-gated checks do not reach — a real, pre-existing gap
// (present before this fix touched anything) filed separately, not
// silently left untested.

BEZEL_TEST(residual_collections_unresolved_member_3896,
           frozenarray_equals_and_gethashcode_identity_spec_8_3_stdlib_4_19) {
    BEZEL_EXPECT_EQ(
        run(program("", identityBodyAnnotated(
                             "FrozenArray<int>", "FrozenArray<int>.From([1, 2, 3])",
                             "FrozenArray<int>.From([4, 5, 6])"))),
        42);
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           frozenarray_tostring_implemented_literal_spec_8_5_stdlib_4_19) {
    const std::string p = program(
        "",
        "  var a: FrozenArray<int> := FrozenArray<int>.From([1, 2, 3]); "
        "  var s := a.ToString(); "
        "  return (if s = \"System.Collections.Generic.FrozenArray<int>\" "
        "          then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           frozenarray_compareto_call_rejected_no_icomparable_stdlib_4_19) {
    const std::string p = program(
        "",
        "  var a1: FrozenArray<int> := FrozenArray<int>.From([1, 2, 3]); "
        "  var a2: FrozenArray<int> := FrozenArray<int>.From([4, 5, 6]); "
        "  var c := a1.CompareTo(a2); "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(
        hasError(p, "no member `CompareTo` on `FrozenArray<T>`"));
    BEZEL_EXPECT_TRUE(noStale2457Citation(p));
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           frozenarray_compareto_bare_reference_also_rejected_stdlib_4_19) {
    const std::string p = program(
        "",
        "  var a1: FrozenArray<int> := FrozenArray<int>.From([1, 2, 3]); "
        "  var f := a1.CompareTo; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(
        hasError(p, "no member `CompareTo` on `FrozenArray<T>`"));
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           frozenarray_bare_tostring_reference_rejected_not_degraded_stdlib_4_19) {
    const std::string p = program(
        "",
        "  var a: FrozenArray<int> := FrozenArray<int>.From([1, 2, 3]); "
        "  var f := a.ToString; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"a bare reference to `ToString`",
            "not yet supported on `FrozenArray<T>`"}));
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           frozenarray_bare_equals_reference_rejected_not_degraded_stdlib_4_19) {
    const std::string p = program(
        "",
        "  var a: FrozenArray<int> := FrozenArray<int>.From([1, 2, 3]); "
        "  var f := a.Equals; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"a bare reference to `Equals`",
            "not yet supported on `FrozenArray<T>`"}));
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           frozenarray_bare_gethashcode_reference_rejected_not_degraded_stdlib_4_19) {
    const std::string p = program(
        "",
        "  var a: FrozenArray<int> := FrozenArray<int>.From([1, 2, 3]); "
        "  var f := a.GetHashCode; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"a bare reference to `GetHashCode`",
            "not yet supported on `FrozenArray<T>`"}));
}

// ═══════════════════════ Dictionary<K,V> (stdlib §4.8) ════════════════════
// `bz_dict_t*`-backed — a genuinely different heap representation from
// OwningBuffer's/FrozenArray's `bz_array_t*`, pinning that the identity
// default's raw-pointer treatment is representation-agnostic.

BEZEL_TEST(residual_collections_unresolved_member_3896,
           dictionary_equals_and_gethashcode_identity_spec_8_3_stdlib_4_8) {
    BEZEL_EXPECT_EQ(
        run(program("", identityBody("new Dictionary<string, int>()",
                                      "new Dictionary<string, int>()"))),
        42);
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           dictionary_tostring_implemented_literal_spec_8_5_stdlib_4_8) {
    const std::string p = program(
        "",
        "  var a := new Dictionary<string, int>(); "
        "  var s := a.ToString(); "
        "  return (if s = "
        "      \"System.Collections.Generic.Dictionary<string, int>\" "
        "          then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           dictionary_compareto_call_rejected_no_icomparable_stdlib_4_8) {
    const std::string p = program(
        "",
        "  var a1 := new Dictionary<string, int>(); "
        "  var a2 := new Dictionary<string, int>(); "
        "  var c := a1.CompareTo(a2); "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(
        hasError(p, "no member `CompareTo` on `Dictionary<K, V>`"));
    BEZEL_EXPECT_TRUE(noStale2457Citation(p));
}

// #2536 (pre-existing, unaffected by `isAuditedElsewhere`) — see the `Set`
// row's explanation. Note the pre-existing diagnostic's family-name
// citation carries NO generic-arity suffix at all ("on `Dictionary`", not
// "on `Dictionary<K, V>`") — distinct from this fix's OWN CompareTo
// carve-out (which DOES render "Dictionary<K, V>" correctly for the CALL
// form, see `dictionary_compareto_call_rejected_no_icomparable_stdlib_4_8`
// above), because the two are different, independent diagnostics.
BEZEL_TEST(residual_collections_unresolved_member_3896,
           dictionary_compareto_bare_reference_also_rejected_stdlib_4_8) {
    const std::string p = program(
        "",
        "  var a1 := new Dictionary<string, int>(); "
        "  var f := a1.CompareTo; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"no member `CompareTo` on `Dictionary`", "documented surface"}));
}

// #2537 (pre-existing, unaffected by `isAuditedElsewhere`) — see the `Set`
// row's explanation.
BEZEL_TEST(residual_collections_unresolved_member_3896,
           dictionary_bare_gethashcode_reference_rejected_not_degraded_stdlib_4_8) {
    const std::string p = program(
        "",
        "  var a := new Dictionary<string, int>(); "
        "  var f := a.GetHashCode; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"a bare reference to `GetHashCode`",
            "not yet supported on `Dictionary`", "#2537"}));
}

// ═══════════════════ OwningDictionary<K,V> (stdlib §4.9) ══════════════════
// `bz_dict_t*`-backed; `where V: class` (Res).

BEZEL_TEST(residual_collections_unresolved_member_3896,
           owningdictionary_equals_and_gethashcode_identity_spec_8_3_stdlib_4_9) {
    BEZEL_EXPECT_EQ(
        run(program(kResClassHelper,
                    identityBody("new OwningDictionary<string, Res>()",
                                 "new OwningDictionary<string, Res>()"))),
        42);
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           owningdictionary_tostring_implemented_literal_spec_8_5_stdlib_4_9) {
    const std::string p = program(
        kResClassHelper,
        "  var a := new OwningDictionary<string, Res>(); "
        "  var s := a.ToString(); "
        "  return (if s = "
        "      \"System.Collections.Generic.OwningDictionary<string, "
        "M.Res>\" "
        "          then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           owningdictionary_compareto_call_rejected_no_icomparable_stdlib_4_9) {
    const std::string p = program(
        kResClassHelper,
        "  var a1 := new OwningDictionary<string, Res>(); "
        "  var a2 := new OwningDictionary<string, Res>(); "
        "  var c := a1.CompareTo(a2); "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(
        hasError(p, "no member `CompareTo` on `OwningDictionary<K, V>`"));
    BEZEL_EXPECT_TRUE(noStale2457Citation(p));
}

// #2537 (pre-existing, unaffected by `isAuditedElsewhere`) — see the `Set`
// row's explanation.
BEZEL_TEST(residual_collections_unresolved_member_3896,
           owningdictionary_bare_tostring_reference_rejected_not_degraded_stdlib_4_9) {
    const std::string p = program(
        kResClassHelper,
        "  var a := new OwningDictionary<string, Res>(); "
        "  var f := a.ToString; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"a bare reference to `ToString`",
            "not yet supported on `OwningDictionary`", "#2537"}));
}

// ═══════════════════════ LinkedList<T> (stdlib §4.6) ══════════════════════
// `bz_llist_t*`-backed — the third distinct heap representation this fix
// pins the identity default against.

BEZEL_TEST(residual_collections_unresolved_member_3896,
           linkedlist_equals_and_gethashcode_identity_spec_8_3_stdlib_4_6) {
    BEZEL_EXPECT_EQ(
        run(program("", identityBody("new LinkedList<int>()",
                                      "new LinkedList<int>()"))),
        42);
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           linkedlist_tostring_implemented_literal_spec_8_5_stdlib_4_6) {
    const std::string p = program(
        "",
        "  var a := new LinkedList<int>(); "
        "  var s := a.ToString(); "
        "  return (if s = \"System.Collections.Generic.LinkedList<int>\" "
        "          then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           linkedlist_compareto_call_rejected_no_icomparable_stdlib_4_6) {
    const std::string p = program(
        "",
        "  var a1 := new LinkedList<int>(); "
        "  var a2 := new LinkedList<int>(); "
        "  var c := a1.CompareTo(a2); "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(
        hasError(p, "no member `CompareTo` on `LinkedList<T>`"));
    BEZEL_EXPECT_TRUE(noStale2457Citation(p));
}

// #2536 (pre-existing, unaffected by `isAuditedElsewhere`) — see the `Set`
// row's explanation.
BEZEL_TEST(residual_collections_unresolved_member_3896,
           linkedlist_compareto_bare_reference_also_rejected_stdlib_4_6) {
    const std::string p = program(
        "",
        "  var a1 := new LinkedList<int>(); "
        "  var f := a1.CompareTo; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"no member `CompareTo` on `LinkedList`", "documented surface"}));
}

// #2537 (pre-existing, unaffected by `isAuditedElsewhere`) — see the `Set`
// row's explanation.
BEZEL_TEST(residual_collections_unresolved_member_3896,
           linkedlist_bare_equals_reference_rejected_not_degraded_stdlib_4_6) {
    const std::string p = program(
        "",
        "  var a := new LinkedList<int>(); "
        "  var f := a.Equals; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"a bare reference to `Equals`",
            "not yet supported on `LinkedList`", "#2537"}));
}

// ═════════════════════ OwningLinkedList<T> (stdlib §4.7) ══════════════════
// `bz_llist_t*`-backed; `where T: class` (Res).

BEZEL_TEST(residual_collections_unresolved_member_3896,
           owninglinkedlist_equals_and_gethashcode_identity_spec_8_3_stdlib_4_7) {
    BEZEL_EXPECT_EQ(
        run(program(kResClassHelper,
                    identityBody("new OwningLinkedList<Res>()",
                                 "new OwningLinkedList<Res>()"))),
        42);
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           owninglinkedlist_tostring_implemented_literal_spec_8_5_stdlib_4_7) {
    const std::string p = program(
        kResClassHelper,
        "  var a := new OwningLinkedList<Res>(); "
        "  var s := a.ToString(); "
        "  return (if s = "
        "      \"System.Collections.Generic.OwningLinkedList<M.Res>\" "
        "          then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(residual_collections_unresolved_member_3896,
           owninglinkedlist_compareto_call_rejected_no_icomparable_stdlib_4_7) {
    const std::string p = program(
        kResClassHelper,
        "  var a1 := new OwningLinkedList<Res>(); "
        "  var a2 := new OwningLinkedList<Res>(); "
        "  var c := a1.CompareTo(a2); "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(
        hasError(p, "no member `CompareTo` on `OwningLinkedList<T>`"));
    BEZEL_EXPECT_TRUE(noStale2457Citation(p));
}

// #2536 (pre-existing, unaffected by `isAuditedElsewhere`) — see the `Set`
// row's explanation.
BEZEL_TEST(residual_collections_unresolved_member_3896,
           owninglinkedlist_compareto_bare_reference_also_rejected_stdlib_4_7) {
    const std::string p = program(
        kResClassHelper,
        "  var a1 := new OwningLinkedList<Res>(); "
        "  var f := a1.CompareTo; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"no member `CompareTo` on `OwningLinkedList`",
            "documented surface"}));
}

// #2537 (pre-existing, unaffected by `isAuditedElsewhere`) — see the `Set`
// row's explanation.
BEZEL_TEST(residual_collections_unresolved_member_3896,
           owninglinkedlist_bare_gethashcode_reference_rejected_not_degraded_stdlib_4_7) {
    const std::string p = program(
        kResClassHelper,
        "  var a := new OwningLinkedList<Res>(); "
        "  var f := a.GetHashCode; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"a bare reference to `GetHashCode`",
            "not yet supported on `OwningLinkedList`", "#2537"}));
}
