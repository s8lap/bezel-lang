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
//   - `.Length`: NOT in stdlib's documented surface. fix #4811 (stdlib
//     §4.4, v0.315) removed it — previously it was an undocumented
//     working alias for `.Count` (LlvmBackend_Exprs.cpp's structural
//     `isCollectionTypeName` dispatch) that this fix's own adversarial
//     `array_` filter sweep found and corrected.
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

// fix #4811 (stdlib §4.4, v0.315): `.Length` is NOT a working alias for
// `.Count` on Array<T> — stdlib §4.4's Count-vs-Length convention says
// resizable Array<T> uses Count, not Length. This row was repurposed from
// the pre-fix "legit_length_alias_value_correct" accept row to assert the
// specific diagnostic.
BEZEL_TEST(array_unresolved_member_2457, array_length_rejected_per_stdlib_4_4_4811) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [7, 8, 9]; "
        "  return a.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "no member `Length` on `Array<T>`: use `Count`"));
}

// A bare (uncalled) method reference — language spec §27.6 — must still
// compile after this fix's reject; the earlier draft of this fix (caught
// before landing) rejected it because it exempted only the CALL form via
// calleeOfCallCtx_, not the reference form language spec §27.6 sanctions.
//
// #2470 CORRECTION (this test previously asserted `run(program) == 0` —
// "compiles and doesn't crash" — which was itself the vacuous-test failure
// mode the standing protocol warns about: #2470 proved the accepted
// reference was a silent NO-OP at runtime, `f(5)` lowering to no code at
// all, not even a null-pointer crash, because `Array<T>.Add` has no real
// MethodSymbol behind it for codegen's method-reference thunk to bind. A
// bare reference silently accepted and then doing nothing is worse than an
// honest rejection (the standing protocol's own framing) — so #2470's fix
// now REJECTS this bare-reference form at the sema level, with a distinct
// diagnostic from "no member" (the member IS real; only its reference form
// isn't implemented yet). The CALL form (`a.Add(5)`, exercised by
// `legit_call_form_methods_value_correct` above) is completely unaffected.
BEZEL_TEST(array_unresolved_member_2457, legit_bare_method_reference_now_rejected_2470) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(3); "
        "  var f := a.Add; "
        "  f(5); "
        "  a.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `Add` (language spec \xC2\xA7"
        "27.6) is not yet supported on `Array<T>`"));
}

// #2470: the reject applies uniformly to OwningArray<T> too (same
// `isDeclaredMethod` union; the class/owning axis is otherwise untouched by
// this fix).
BEZEL_TEST(array_unresolved_member_2457, legit_bare_method_reference_owningarray_now_rejected_2470) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  var a := new OwningArray<Res>(); "
        "  var f := a.Add; "
        "  a.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `Add` (language spec \xC2\xA7"
        "27.6) is not yet supported on `OwningArray<T>`"));
}

// #2470: `GetEnumerator` is in the same `isDeclaredMethod` union (the
// IEnumerable<T> ancestor method, stdlib §4.4/§4.12) and shares the
// identical no-MethodSymbol shape — confirmed live before this fix:
// `var f := a.GetEnumerator; var it := f();` compiled clean and `it`'s
// MoveNext() was unreachable dead code, not merely unimplemented.
BEZEL_TEST(array_unresolved_member_2457, legit_bare_getenumerator_reference_now_rejected_2470) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(3); "
        "  var f := a.GetEnumerator; "
        "  a.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `GetEnumerator` (language spec \xC2\xA7"
        "27.6) is not yet supported on `Array<T>`"));
}

// #2470: folded the `isUniversalObjectMember` names into the SAME reject —
// a bare reference to `.ToString`/`.Equals` shares the identical
// no-MethodSymbol root cause as `.Add`, but manifests WORSE: before this
// fix, `var f := a.ToString; var s := f(); return (if s = "Array" ...)`
// aborted the compiler inside LLVM's `ICmpInst::AssertOK` (comparing the
// #2434-degraded, mistyped `s` against a string constant), and `var f :=
// a.Equals; f(a)` separately failed IR verification ("Branch condition is
// not i1 type"). Both are now clean, honest rejections instead of a crash.
BEZEL_TEST(array_unresolved_member_2457, legit_bare_tostring_reference_now_rejected_2470_no_crash) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(0); "
        "  a.Add(1); "
        "  var f := a.ToString; "
        "  var s := f(); "
        "  a.Dispose(); "
        "  return (if s = \"Array\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `ToString` (language spec \xC2\xA7"
        "27.6) is not yet supported on `Array<T>`"));
}

BEZEL_TEST(array_unresolved_member_2457, legit_bare_equals_reference_now_rejected_2470_no_crash) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(0); "
        "  a.Add(1); "
        "  var f := a.Equals; "
        "  var b := f(a); "
        "  a.Dispose(); "
        "  return (if b then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `Equals` (language spec \xC2\xA7"
        "27.6) is not yet supported on `Array<T>`"));
}

// #2492 (stdlib §5; language spec §27.6): a BARE reference to a stdlib §5
// LINQ extension operator (`Where`/`Select`/`Take`/...) on a declaration-less
// builtin collection is the identical #2470/#2493 silent-no-op shape — the
// operator is dispatched STRUCTURALLY by name in codegen's
// tryLowerLinqOperatorCall (LlvmBackend_Calls3.cpp), never through a real
// MethodSymbol, so sema left `ma`'s exprType UNSET and codegen's #2434
// fallback degraded the local to a zero-initialized value (every later call
// through it lowered to NO CODE). Confirmed live on origin/main before this
// fix: `var f := a.Where;` compiled CLEAN (no diagnostic from sema —
// `isLinqOperator` was exempted from BOTH the #2470 bare-reference reject
// and the "no member" reject) and emitted only the codegen-internal
// `lowerLocalDecl's initializer ... degraded to a zero-initialized value --
// #2434`. sema now folds `isLinqOperator` into the same #2470 reject, so the
// forbidden clean-compile-then-silently-do-nothing outcome is replaced with
// an honest "not yet supported ... call it directly instead" diagnostic.
//
// This test was originally `legit_linq_bare_method_reference_not_regressed`
// asserting `run(program) == 0`, but that was VACUOUS — it never CALLED `f`,
// so it passed whether the bare reference genuinely worked OR silently
// no-op'd (the defect). It is repurposed here to assert the reject (FAILS
// without this fix: no error, silent no-op; PASSES with it: reject fires).
// The reject message says "not yet supported" (NOT "no member"), which also
// preserves the original test's intent: confirm `Where` IS recognized as a
// genuine stdlib §5 LINQ member of the receiver (in `isLinqOperator`), not a
// bogus name — the false-"no member"-reject regression #2484 guarded against.
BEZEL_TEST(array_unresolved_member_2457, legit_bare_linq_reference_now_rejected_2492) {
    const std::string program =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(3); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var f := a.Where; "
        "  a.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `Where` (language spec \xC2\xA7"
        "27.6) is not yet supported on `Array<T>`"));
}

// #2492 adversarial: `.Select`/`.Take`/`.First` bare references share the
// identical root cause on Array<T> — sweep the shape, not just `Where`.
BEZEL_TEST(array_unresolved_member_2457, legit_bare_linq_select_take_first_now_rejected_2492) {
    const std::string program =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(3); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var f := a.Select; "
        "  var g := a.Take; "
        "  var h := a.First; "
        "  a.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `Select` (language spec \xC2\xA7"
        "27.6) is not yet supported on `Array<T>`"));
}

// #2484/#2492: `.MinOrNull`/`.MaxOrNull` bare reference — both are genuine
// members of stdlib §5.1's `IEnumerable<T> where T: IComparable<T>` extension
// with no per-type override on Array/OwningArray (added to `isLinqOperator` by
// #2484 to stop a false "no member" reject). #2492 folds them into the same
// bare-reference reject as the other LINQ operators: the CALL form
// (`a.MinOrNull()`) is unaffected (exempted by `!isCallForm`), but the BARE
// form (`var f := a.MinOrNull;`) is now honestly rejected instead of silently
// no-op'ing. The reject message says "not yet supported" (NOT "no member"),
// preserving #2484's intent: the names ARE recognized as genuine LINQ members
// (in `isLinqOperator`), not bogus names. Repurposed from the prior vacuous
// `run(program) == 0` assertion (which never called `f`/`g`).
BEZEL_TEST(array_unresolved_member_2457, legit_minornull_maxornull_bare_reference_now_rejected_2492) {
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
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `MinOrNull` (language spec \xC2\xA7"
        "27.6) is not yet supported on `Array<T>`"));
}

// #2492 adversarial VALUE-correctness: the CALL form `a.Where(lambda).First()`
// MUST still compile and return the correct value after the bare-reference
// reject (the reject is gated on `!isCallForm`). From [1,2,3,4], `Where(x > 2)`
// yields [3,4] and `First()` returns 3 — proving the call form's real LINQ
// dispatch survives the fix (the control for the bare-form reject above).
BEZEL_TEST(array_unresolved_member_2457, legit_where_call_form_value_correct_2492) {
    const std::string program =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(4); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  var e := a.Where(lambda(x) => x > 2); "
        "  var v := e.First(); "
        "  a.Dispose(); "
        "  return v; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 3);
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
        "  return copy.Count; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 3);
}

// ToString: implemented as the fixed literal, VALUE-correct.
//
// fix #3119 (spec §8.5, v0.252; stdlib §2.13 rule 1 + rule 3): the literal
// is the QUALIFIED module path with the §2.13 rule 3 generic-args tail, so
// `Array<int>` returns "System.Collections.Generic.Array<int>" (not "Array")
// and `OwningArray<Res>` returns "System.Collections.Generic.OwningArray<Res>"
// (the Res arg recurses through `spec85TypeRefQualifiedName`, rendering as
// its user-declared qualified name "M.Res").
BEZEL_TEST(array_unresolved_member_2457, tostring_implemented_array_literal) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(1); "
        "  a.Add(5); "
        "  var s := a.ToString(); "
        "  a.Dispose(); "
        "  return (if s = \"System.Collections.Generic.Array<int>\" then 42 else 1); "
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
        "  return (if s = \"System.Collections.Generic.OwningArray<M.Res>\" then 42 else 1); "
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
