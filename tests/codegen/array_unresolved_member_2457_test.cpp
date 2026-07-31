// Regression tests for issue #2457 slice 1 — the same declaration-less-
// builtin-type silent-fallthrough shape #2444/#2458/#2459 fixed for
// IEnumerator<T>/DictionaryEntry<K,V>, applied to `Array<T>`/`OwningArray<T>`
// (stdlib §4.4/§4.5). Confirmed live on origin/main before this fix:
//
//   var a := new Array<int>(3); a.Add(14);
//   var x := a.TotallyBogusArrayMember123;
//
// compiled at exit 0 with zero diagnostics. #2457's own filing (under
// #2444) found this is NOT specific to `Dictionary<K,V>.Keys`/`.Values` (the
// category that started the investigation) — it is the whole array-backed
// builtin-collection family sharing the identical gap.
//
// This slice covers ONLY `Array<T>`/`OwningArray<T>`, per the coordinator's
// explicit direction to land the collection-family sweep as separate,
// independently-landable slices (Stack/Queue and Dictionary/Set are
// follow-up work, not attempted here).
//
// Per-member disposition (verified against docs/Bezel_Standard_Library.md
// §4.4/§4.5/§5, not inferred from what the compiler special-cases):
//   - Count/Capacity: already typed by a pre-existing property handler.
//   - `.Length`: NOT in stdlib's documented surface, but a real, WORKING,
//     tested alias for `.Count` (LlvmBackend_Exprs.cpp's structural
//     `isCollectionTypeName` dispatch) — caught by this fix's own
//     adversarial `array_` filter sweep (55 failures) before landing;
//     exempted, not implemented (it needed no new implementation).
//   - Every declared method (stdlib §4.4/§4.5) and §5 LINQ operator: a
//     CALL is exempted via `calleeOfCallCtx_`; language spec §27.6 also
//     sanctions a BARE reference to an instance method with no call
//     (`var f := a.Add;`), so an explicit name list (transcribed fresh
//     from stdlib, see the sema comment) exempts the bare-reference case
//     too — a gap a live probe caught before landing (no existing test
//     exercises a bare Array-method reference).
//   - ToString/Equals/GetHashCode: `Array<T>`/`OwningArray<T>` are real,
//     documented `public class` types (stdlib §4.4/§4.5) with no override
//     shown, and are genuine heap objects (`bz_array_t*`) with stable
//     identity — the language spec §8.3 "Class" default (reference
//     equality / identity hash) and §8.5 default (qualified name, a
//     compile-time constant here since both are real, nameable types)
//     apply mechanically. IMPLEMENTED (not diagnosed), unlike the harder
//     cases #2459 tracks (DictionaryEntry's field-by-field record default;
//     DrainedEntry's Equals/GetHashCode, which cannot be implemented
//     soundly because its runtime representation has no stable identity).
//   - GetType/ReferenceEquals: exempted, matching every other category's
//     treatment (unaffected by this fix either way).

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

// ── Bogus member rejected ───────────────────────────────────────────────

BEZEL_TEST(array_unresolved_member_2457, bogus_property_on_array_rejected_stdlib_4_4) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(3); "
        "  a.Add(14); "
        "  var x := a.TotallyBogusArrayMember123; "
        "  a.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "no member `TotallyBogusArrayMember123` on `Array<T>`"));
}

BEZEL_TEST(array_unresolved_member_2457, bogus_property_on_owningarray_rejected_stdlib_4_5) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  var a := new OwningArray<Res>(); "
        "  a.Add(new Res(1)); "
        "  var x := a.TotallyBogusOwningArrayMember; "
        "  a.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "no member `TotallyBogusOwningArrayMember` on `OwningArray<T>`"));
}

// ── Legitimate members keep working, VALUE-correct ──────────────────────

// `.Length` — the undocumented-but-real, working alias for `.Count`.
BEZEL_TEST(array_unresolved_member_2457, legit_length_alias_value_correct) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [7, 8, 9]; "
        "  return a.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 3);
}

// A bare (uncalled) method reference — language spec §27.6 — must still
// compile after this fix's reject; the earlier draft of this fix (caught
// before landing) rejected it because it exempted only the CALL form via
// calleeOfCallCtx_, not the reference form language spec §27.6 sanctions.
BEZEL_TEST(array_unresolved_member_2457, legit_bare_method_reference_not_regressed) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(3); "
        "  var f := a.Add; "
        "  f(5); "
        "  a.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 0);
}

// A LINQ bare method reference (requires the §5 import) — the largest,
// least-obvious slice of the exempt list.
BEZEL_TEST(array_unresolved_member_2457, legit_linq_bare_method_reference_not_regressed) {
    const std::string program =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(3); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var f := a.Where; "
        "  a.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 0);
}

// #2484: `.MinOrNull`/`.MaxOrNull` bare reference — both are genuine members
// of stdlib §5.1's `IEnumerable<T> where T: IComparable<T>` extension with
// no per-type override on Array/OwningArray, but were missing from this
// slice's own `isLinqOperator` list (found by independently re-deriving the
// whole §5/§5.1 surface against the doc, not by diffing against slice 2's
// already-corrected Stack/Queue list — the same "MinOrNull` bare reference
// falsely rejected" shape slice 2's own review caught in its own list,
// commit bd41a69). Confirmed on origin/main before this fix: `var f :=
// a.MinOrNull;` was rejected with "no member `MinOrNull` on `Array<T>`".
BEZEL_TEST(array_unresolved_member_2457, legit_minornull_maxornull_bare_method_reference_not_regressed) {
    const std::string program =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(3); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var f := a.MinOrNull; "
        "  var g := a.MaxOrNull; "
        "  a.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 0);
}

// Call-form methods across the declared surface still work — sampled, not
// exhaustive (the broad `array_`/`linq`/`collection` suites, ~1600 tests,
// already exercise the full call surface; this is a direct guard on THIS
// fix's own reject condition).
BEZEL_TEST(array_unresolved_member_2457, legit_call_form_methods_value_correct) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(3); "
        "  a.Add(3); a.Add(1); a.Add(2); "
        "  a.Sort(lambda(x: int, y: int): int => x - y); "
        "  var found := a.Contains(2); "
        "  var idx := a.IndexOf(3); "
        "  using var copy := a.ToArray(); "
        "  a.Dispose(); "
        "  if not found then return 1; end; "
        "  if idx <> 2 then return 2; end; "     // sorted: [1,2,3] -> 3 at index 2
        "  return copy.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 3);
}

// ToString: implemented as the fixed literal, VALUE-correct.
BEZEL_TEST(array_unresolved_member_2457, tostring_implemented_array_literal) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(1); "
        "  a.Add(5); "
        "  var s := a.ToString(); "
        "  a.Dispose(); "
        "  return (if s = \"Array\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

BEZEL_TEST(array_unresolved_member_2457, tostring_implemented_owningarray_literal) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  var a := new OwningArray<Res>(); "
        "  a.Add(new Res(1)); "
        "  var s := a.ToString(); "
        "  a.Dispose(); "
        "  return (if s = \"OwningArray\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// Equals/GetHashCode: implemented via direct identity, VALUE-correct
// (self-equal, two distinct arrays unequal, hash stable across calls).
BEZEL_TEST(array_unresolved_member_2457, equals_and_gethashcode_identity_semantics_spec_8_3) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a1 := new Array<int>(1); "
        "  var a2 := new Array<int>(1); "
        "  var selfEq := a1.Equals(a1); "
        "  var crossEq := a1.Equals(a2); "
        "  var h1a := a1.GetHashCode(); "
        "  var h1b := a1.GetHashCode(); "
        "  a1.Dispose(); a2.Dispose(); "
        "  if not selfEq then return 1; end; "
        "  if crossEq then return 2; end; "
        "  if h1a <> h1b then return 3; end; "
        "  return 42; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}
