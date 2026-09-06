// Regression tests for issue #2465 — ExprTC::resolveMemberAccess's "issue
// #605" bare-type-parameter block (src/sema/ExprTypecheck_Calls.cpp) had
// the exact false-match shape #2433 was retracted for: its guard
// (`kind==Named && !declaration && astNode->name->parts.size()==1`) matched
// ANY declaration-less builtin Named type with a single-part synthetic
// name — not just a genuinely-declared generic type parameter — and
// unconditionally typed ToString/GetHashCode/Equals/CompareTo/GetType for
// all of them with no `isDeclaredParam`-style narrowing at all.
//
// Confirmed live on origin/main before this fix, in two distinct failure
// shapes:
//   1. `Stack<int>.Equals(itself)` silently returned `false` (a wrong
//      value, not a crash) — #605's own fabricated `bool` typing let the
//      call compile, but nothing implements Stack's actual Equals.
//   2. This masked #2458/#2459's and #2457 slice 1's own new diagnostics
//      for IEnumerator<T>/DictionaryEntry<K,V>/Array<T>/OwningArray<T>
//      until those blocks were reordered to run before this one.
//
// The fix narrows #605's guard with the SAME `isDeclaredParam` scan #1078
// already established (a scan of the enclosing method's/class's own
// declared `genericParams` for a name match) — confirming the receiver is
// GENUINELY a declared type parameter, not merely shaped like one.
//
// Narrowing alone was proven UNSAFE by this fix's own adversarial pass
// before landing: with #605 no longer typing these five names for
// Stack<T>/Queue<T>/Set<T>/LinkedList<T>/Dictionary<K,V>/..., `var eq :=
// stack.Equals(stack); if eq then ... end;` IR-verification-CRASHED the
// compiler itself ("Branch condition is not 'i1' type!") — a live,
// mechanically-reproduced hazard, not a theoretical one, because codegen's
// own separate Equals dispatch still computes and stores a real value
// while sema's local-slot-type inference silently defaults to `i32`. The
// fix therefore ALSO adds a second, precisely-scoped block: for exactly
// the canonical `isArrayBackedCollectionName`/`isDictionaryName`/
// `isLinkedListName` registries (common/BuiltinRegistry.hpp — the SAME
// ones codegen itself uses), minus `Array`/`OwningArray` (already
// implemented, #2457 slice 1), a compile-time "declared via Object but
// not yet implemented (tracked #2457)" diagnostic replaces the crash. A
// FIRST DRAFT of that second block used a general structural test instead
// of this precise registry check and wrongly rejected `ReadOnlyStringSpan.
// ToString()` (a real, tested, working feature —
// tests/codegen/string_span_883_test.cpp) and IEnumerator<T>/Array<T>'s
// own newly-implemented Equals/GetHashCode/ToString (implemented directly
// in codegen this session, deliberately WITHOUT setting sema's exprType —
// the same shape as #2423's TypeInitializationException.InnerException
// exemption); caught by running the broad `span`/`array_`/`iter_` filters
// before landing, not by a narrow one.

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

// ── The exact crash repro: now a clean compile-time reject ──────────────

// #2465 core repro: `Stack<int>.Equals(itself)` used in a branch condition
// used to IR-verification-crash the compiler after a naive #605 narrowing
// (confirmed mechanically during this fix's own development, before the
// second block was added). At the time #2465 landed, Stack/Queue had no
// audited implementation yet, so this asserted the PLACEHOLDER "declared
// via Object... not yet implemented (tracked #2457)" reject as the
// categorically-safer-than-a-crash interim behavior.
//
// #2457 slice 2 is exactly the "tracked #2457" follow-up #2465's own commit
// message named: it audits Stack/OwningStack/Queue/OwningQueue (stdlib
// §4.11) and IMPLEMENTS Equals/GetHashCode/ToString directly (§8.3/§8.5
// mechanical defaults — real heap objects, no override shown), so
// `isAuditedElsewhere` in the #2465 block now excludes these four names and
// this placeholder reject no longer fires for them. Updated here (rather
// than left red) per that slice's own explicit anticipation of this exact
// change — not a scope violation of #2465's original PR, but its own
// stated follow-up landing. The crash-safety property this test was
// written to prove is now demonstrated the STRONGER way: not just "doesn't
// crash" but "computes the spec-correct value."
BEZEL_TEST(collection_universal_member_narrowing_2465,
           stack_equals_used_in_branch_now_implemented_value_correct) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  s.Push(1); "
        "  var eq := s.Equals(s); "
        "  s.Dispose(); "
        "  if eq then return 42; else return 1; end; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// Stack<T>.ToString() — was: compiled clean, segfaulted on use (the
// original #2465 filing's own repro), then (after #2465 landed) a clean
// placeholder reject. Now implemented (#2457 slice 2, see above): the §8.5
// default for a `public class` with no ToString override is the type's
// qualified name — fix #3119 makes the literal the QUALIFIED module path
// with the §2.13 rule 3 generic-args tail:
// "System.Collections.Generic.Stack<int>" (was "Stack").
BEZEL_TEST(collection_universal_member_narrowing_2465,
           stack_tostring_now_implemented_value_correct) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  s.Push(1); "
        "  var x := s.ToString(); "
        "  s.Dispose(); "
        "  return (if x = \"System.Collections.Generic.Stack<int>\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// Stack<T>.GetHashCode() — was: silently returned 0, then (after #2465) a
// clean placeholder reject. Now implemented (#2457 slice 2): the §8.3
// identity-hash default via bz_obj_identity_hash — stable across repeated
// calls on the same object.
BEZEL_TEST(collection_universal_member_narrowing_2465,
           stack_gethashcode_now_implemented_value_correct) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  s.Push(1); "
        "  var h1 := s.GetHashCode(); "
        "  var h2 := s.GetHashCode(); "
        "  s.Dispose(); "
        "  return (if h1 = h2 then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// The universal-member coverage generalizes across the registry, not just
// Stack — Set (array-backed, hash-table-on-top) and Dictionary<K,V> itself
// (dict-backed) each confirmed independently. Queue<T> — array-backed, and
// in #2457 slice 2's scope — moves to the positive/implemented assertion
// alongside Stack above, mirroring exactly.
//
// fix-forward (spec §8.3, v0.294): this test's Set/Dictionary rows used to
// pin them as REMAINING rejected (neither was in #2457 slice 2's scope of
// Stack/OwningStack/Queue/OwningQueue). `narrow #3896` (audit Set/OwningSet/
// SortedSet/FrozenArray/Dictionary/OwningDictionary/LinkedList/
// OwningLinkedList for §8.3/§8.5) closed exactly that gap — both are now
// among the eight names `isAuditedElsewhere` admits, joined to the same
// §8.3 Class-kind identity default (LlvmBackend_Calls2.cpp's
// `isBuiltinIdentityResidualCollection`) Stack/Queue already had. MEASURED
// on this tree: both programs below now compile and run clean where the
// pre-#3896 compiler rejected them with exactly the `hasError` text this
// row used to assert — STALE, mirroring the Stack/Queue update above.
// Replaced with the same "computes the spec-correct value" VALUE
// assertions Stack/Queue got: identity `GetHashCode` is stable across two
// calls on the SAME `Set<int>` and DIFFERS from a distinct, separately-
// constructed `Set<int>` with the same element (ruling out a degenerate
// constant-zero default, #2804's trap); `Equals` on `Dictionary<K,V>` is
// reference equality (true against itself, false against a distinct,
// separately-constructed `Dictionary` with the same entries).
BEZEL_TEST(collection_universal_member_narrowing_2465,
           universal_member_now_implemented_generalizes_across_the_registry_spec_8_3) {
    // Queue<T> (#2457 slice 2): now implemented, not rejected.
    //
    // fix #3119 (spec §8.5, v0.252; stdlib §2.13 rule 1 + rule 3): the literal
    // is the QUALIFIED module path with the §2.13 rule 3 generic-args tail
    // — "System.Collections.Generic.Queue<int>" (was "Queue").
    {
        const std::string program =
            "module M; "
            "class C public static method Main(): int "
            "  var q := new Queue<int>(); "
            "  q.Enqueue(1); "
            "  var x := q.ToString(); "
            "  q.Dispose(); "
            "  return (if x = \"System.Collections.Generic.Queue<int>\" then 42 else 1); "
            "end Main; end C;";
        BEZEL_EXPECT_EQ(run(program), 42);
    }
    // Set<T> (narrow #3896): §8.3 identity default, VALUE-correct.
    {
        const std::string program =
            "module M; "
            "class C public static method Main(): int "
            "  var st := new Set<int>(); "
            "  st.Add(1); "
            "  var st2 := new Set<int>(); "
            "  st2.Add(1); "
            "  var h1 := st.GetHashCode(); "
            "  var h2 := st.GetHashCode(); "
            "  var h3 := st2.GetHashCode(); "
            "  var ok := h1 = h2 and h1 <> h3; "
            "  st.Dispose(); "
            "  st2.Dispose(); "
            "  return (if ok then 42 else 1); "
            "end Main; end C;";
        BEZEL_EXPECT_EQ(run(program), 42);
    }
    // Dictionary<K,V> (narrow #3896): §8.3 identity default, VALUE-correct.
    {
        const std::string program =
            "module M; "
            "class C public static method Main(): int "
            "  var d := new Dictionary<string, int>(); "
            "  d[\"a\"] := 1; "
            "  var e := new Dictionary<string, int>(); "
            "  e[\"a\"] := 1; "
            "  var ok := d.Equals(d) and not d.Equals(e); "
            "  d.Dispose(); "
            "  e.Dispose(); "
            "  return (if ok then 42 else 1); "
            "end Main; end C;";
        BEZEL_EXPECT_EQ(run(program), 42);
    }
}

// A truly bogus (non-universal) member on Stack was UNAFFECTED by #2465
// (silently accepted, no crash, since nothing computes or stores a value
// for it) — #2465's own comment explicitly named closing this residual gap
// as "#2457 slice 2's job, not this fix's". Slice 2 closes it: Stack's full
// stdlib §4.11 surface is now audited, so a genuinely nonexistent member is
// rejected with a "no member" diagnostic (ExprTypecheck_Calls.cpp, the new
// #2457-slice-2 block) instead of silently falling through.
BEZEL_TEST(collection_universal_member_narrowing_2465,
           bogus_non_universal_member_now_rejected_by_2457_slice_2) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  s.Push(1); "
        "  var x := s.TotallyBogusStackMember; "
        "  s.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program, "no member `TotallyBogusStackMember` on `Stack<T>`"));
}

// GetType() is deliberately EXCLUDED from this fix's reject list: it is
// already diagnosed, for EVERY receiver type uniformly, by the pre-existing
// "reflection is not yet implemented" gate (§8.2, #487) — adding a second,
// differently-worded rejection here would be a message regression, not a
// new correctness fix.
BEZEL_TEST(collection_universal_member_narrowing_2465,
           gettype_on_stack_still_uses_reflection_gate_not_this_fixs_wording) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  s.Push(1); "
        "  var t := s.GetType(); "
        "  s.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "reflection is not yet implemented: 'GetType()'"));
}

// `ReadOnlyStringSpan.ToString()` (stdlib §3.3, via `string.AsSpan`) is a
// real, pre-existing, tested feature (string_span_883_test.cpp) that a
// first draft of this fix's second block wrongly rejected (it is NOT a
// member of the array/dict/linked-list registries this fix scopes to).
// VALUE-correct, not just clean compile: materializes the exact substring.
BEZEL_TEST(collection_universal_member_narrowing_2465,
           readonlystringspan_tostring_not_regressed_value_correct) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := \"hello world\".AsSpan(6, 5).ToString(); "
        "  return (if s = \"world\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── The positive direction: a genuine bare type-parameter receiver ──────

// A genuinely-declared generic type parameter `T` (here substituted with
// `int`, a value type) calling ToString/GetHashCode/Equals must behave
// IDENTICALLY to before this fix — the narrowed #605 block still types
// these for it (isDeclaredParam is true), and codegen's
// genericSubstSemaTy_-based deferred resolution (unaffected by this fix,
// which touches only sema's own gate) still computes the real values.
// VALUE-correct, not just clean compile — the coordinator's own explicit
// ask, and the exact shape #2433 broke (generic_record_field_590).
//
// CompareTo is deliberately NOT exercised here (it originally was, as
// `val.CompareTo(val)` — a SELF-compare, so its `cmp <> 0` check passed
// vacuously whether CompareTo really dispatched or, per #2480, silently
// returned a hardcoded `0` for every pair regardless of the receiver —
// `where T: value` alone never implied `IComparable<T>`). #2480 fixed
// this: a bare-T `.CompareTo` call is now a compile-time reject unless T's
// constraints actually establish `IComparable<T>` conformance (§14.2),
// exactly like any other member #1078 already gates on an unconstrained
// type parameter. See generic_constraint_dispatch_484_test.cpp /
// record_generic_constraint_test.cpp for the genuine
// `where T: IComparable<T>` dispatch coverage, and
// generic_bare_typeparam_compareto_reject_2480_test.cpp for this issue's own
// reject-diagnostic regression test.
BEZEL_TEST(collection_universal_member_narrowing_2465,
           genuine_type_parameter_unaffected_value_correct) {
    const std::string program =
        "module M; "
        "class C "
        "  public static method F<T>(val: T): int where T: value "
        "    var s := val.ToString(); "
        "    var h := val.GetHashCode(); "
        "    var eq := val.Equals(val); "
        "    if s <> \"7\" then return 1; end; "
        "    if not eq then return 2; end; "
        "    return h; "  // int.GetHashCode() is its own value, per stdlib §9
        "  end F; "
        "  public static method Main(): int "
        "    return F(7); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 7);
}

// A genuine type parameter substituted with a RECORD (value-shaped, not
// primitive) also stays unaffected — this is the exact receiver category
// generic_record_field_590 exercises for field access; a direct guard
// here on `.Equals()` for the same substitution shape. (`.ToString()` on
// a record-substituted bare-T receiver was found, while writing this
// test, to be an INDEPENDENT, PRE-EXISTING crash reproducing identically
// with this fix's sema change fully reverted — filed as #2469, not
// exercised here to avoid encoding a known-separate bug as expected
// behavior in this fix's own suite.)
BEZEL_TEST(collection_universal_member_narrowing_2465,
           genuine_type_parameter_record_substitution_value_correct) {
    const std::string program =
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method F<T>(val: T): bool where T: value "
        "    return val.Equals(val); "
        "  end F; "
        "  public static method Main(): int "
        "    var eq := F(new Pt(3, 4)); "
        "    return (if eq then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}
