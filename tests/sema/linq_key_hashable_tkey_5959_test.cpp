// fix #5959 (stdlib §5 `GroupBy<TKey>` / `ToDictionary<TKey[, TValue]>`,
// spec §14.2 + §3.6 hashable row clause (e), v0.338): the member-level
// `where TKey: hashable` those three stdlib §5 members state is enforced at
// the call site. Pre-fix neither the GroupBy arm nor any of the three
// ToDictionary arms consulted the §3.6 `hashable` computation, so a
// function-typed key compiled and ran — a silent-acceptance inversion: the
// failure surfaced as a wrong answer inside a Dictionary/IGrouping rather
// than as a diagnostic.
//
// Spec lines (docs/Bezel_Language_Spec.md, v0.338):
//   §3.6 hashable row, clause (e) (line 484): "(e) a function type
//     ([§27.7.1]: a lambda equals only itself, so a hash container of
//     function values would hold every lambda evaluation apart; ...)".
//   §14.2 "A type parameter as the type argument" (line 2389): "An
//     unsatisfied constraint is a compile error at that site, naming the
//     constraint, the argument and the parameter it turns on".
//   §3.6 rule 4 (line 497): the diagnostic names the property in the words
//     of the table above.
// Stdlib §5 (docs/Bezel_Standard_Library.md, v0.338):
//   line 3697 `GroupBy<TKey>(key: (T) -> TKey): … where TKey: hashable;`
//   lines 3701-3702 both `ToDictionary` overloads, same `where TKey: hashable`.
//   line 3742 "Key and equality contracts": these three "state the same
//     requirement on `TKey` with an ordinary member-level `where` clause …
//     Both are the ordinary constraint form of language spec §14.2 — on a
//     §26.4 extension block in the first case, on the member's own type
//     parameter in the second".
//
// The five typing arms are covered separately below (two GroupBy receivers,
// three ToDictionary receivers), because #3730's history on the six
// constrained-block operators is exactly the "one site is not the other
// site" defect: a gate present on some arms and absent on others is a
// silent acceptance on the rest.
//
// CONTROLS (must stay accepted — a `where`-clause check that fires too
// eagerly is its own defect): the near-miss is a HASHABLE key selector at
// the same three receivers and the same two members. Every control below
// compiles, links and runs; a rejection-only test set would also pass if the
// check were `return false` unconditionally.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// Every rejection row asserts ALL THREE of §14.2's required nouns — the
// constraint (`where TKey: hashable`), the ARGUMENT (`(int) -> int`, not a
// `?`), and the parameter it turns on (`TKey`) — plus §3.6 clause (e)'s
// structural reason. Dropping any one of them would let a diagnostic that
// names nothing still pass.
static void expectTKeyHashableRejection(const bezel::test::JitResult& r,
                                        const char* op) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, op));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "TKey"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`where TKey: hashable`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "(int) -> int"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "function type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "clause (e)"));
}

// ── T1 — §14.2 + §3.6 clause (e): `Array<T>.ToDictionary` keyed by a
// function type. This is the concrete-collection ToDictionary arm.
BEZEL_TEST(sema_linq_key_hashable_tkey_5959,
           todictionary_function_key_rejected_stdlib_5_member_where_clause) {
    auto r = runAot(
        "module M5959A; "
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run() "
        "    var a := new Array<int>(2); "
        "    a.Add(1); "
        "    using var d := a.ToDictionary(lambda(x: int): (int) -> int => Demo.Twice); "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return 0; end Main; end C;");
    expectTKeyHashableRejection(r, "ToDictionary");
}

// ── T2 — the LAZY-receiver GroupBy arm (the other GroupBy spelling).
BEZEL_TEST(sema_linq_key_hashable_tkey_5959,
           groupby_function_key_rejected_stdlib_5_member_where_clause) {
    auto r = runAot(
        "module M5959B; "
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run() "
        "    var a := new Array<int>(2); "
        "    a.Add(1); "
        "    var n := a.GroupBy(lambda(x: int): (int) -> int => Demo.Twice).Count(); "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return 0; end Main; end C;");
    expectTKeyHashableRejection(r, "GroupBy");
}

// ── T3 — the LAZY `IEnumerable<T>` receiver arms (a chain feeds the call,
// so it routes through `typeLinqOperator`'s ToDictionary, a DIFFERENT if/
// else-if chain from T1's). #3730's finding: the two chains are separate and
// a gate on one is not a gate on the other.
BEZEL_TEST(sema_linq_key_hashable_tkey_5959,
           lazy_receiver_todictionary_function_key_rejected) {
    auto r = runAot(
        "module M5959C; "
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run() "
        "    var a := new Array<int>(2); "
        "    a.Add(1); "
        "    using var d := a.Where(lambda(x: int): bool => x > 0)"
        "                         .ToDictionary(lambda(x: int): (int) -> int => Demo.Twice); "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return 0; end Main; end C;");
    expectTKeyHashableRejection(r, "ToDictionary");
}

BEZEL_TEST(sema_linq_key_hashable_tkey_5959,
           lazy_receiver_groupby_function_key_rejected) {
    auto r = runAot(
        "module M5959D; "
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run() "
        "    var a := new Array<int>(2); "
        "    a.Add(1); "
        "    var n := a.Where(lambda(x: int): bool => x > 0)"
        "                    .GroupBy(lambda(x: int): (int) -> int => Demo.Twice)"
        "                    .Count(); "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return 0; end Main; end C;");
    expectTKeyHashableRejection(r, "GroupBy");
}

// ── T4 — the STRING (`IEnumerable<char>`, #1220) ToDictionary arm. A third
// spelling of the same member, in a third receiver block.
BEZEL_TEST(sema_linq_key_hashable_tkey_5959,
           string_receiver_todictionary_function_key_rejected) {
    auto r = runAot(
        "module M5959E; "
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run() "
        "    using var d := \"ab\".ToDictionary(lambda(c: char): (int) -> int => Demo.Twice); "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return 0; end Main; end C;");
    expectTKeyHashableRejection(r, "ToDictionary");
}

// ── T5 — §3.6 clause (c) over clause (e): a NULLABLE function-type key
// `((int) -> int)?` is not hashable either, and the check says so while
// still naming the argument.
BEZEL_TEST(sema_linq_key_hashable_tkey_5959,
           nullable_function_key_rejected_spec_3_6_clause_c_e) {
    auto r = runAot(
        "module M5959F; "
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run() "
        "    var a := new Array<int>(2); "
        "    a.Add(1); "
        "    using var d := a.ToDictionary(lambda(x: int): ((int) -> int)? => Demo.Twice); "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`where TKey: hashable`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "function type"));
}

// ── T6 — NEAR-MISS CONTROL. A hashable key selector over the same three
// receivers and the same two members still compiles, links and runs. This
// is the row that makes the rejection rows above meaningful: it is the
// other side of the boundary, and it exercises the check's ACCEPT path at
// each arm the rejections reach.
BEZEL_TEST(sema_linq_key_hashable_tkey_5959,
           hashable_key_selector_still_accepted_control) {
    auto r = runAot(
        "module M5959G; "
        "static class Demo "
        "  public static method Key(n: int): string => \"k\"; "
        "  public static method Label(c: char): string => $\"k{c}\"; "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run(): int "
        "    var a := new Array<int>(2); "
        "    a.Add(1); "
        // concrete-collection ToDictionary (T1's arm)
        "    using var d := a.ToDictionary(lambda(x: int): string => Demo.Key(x)); "
        "    if d.Count <> 1 then return 10; end if; "
        // lazy-receiver ToDictionary (T3's arm)
        "    using var d2 := a.Where(lambda(x: int): bool => x > 0)"
        "                          .ToDictionary(lambda(x: int): string => Demo.Key(x)); "
        "    if d2.Count <> 1 then return 11; end if; "
        // string-receiver ToDictionary (T4's arm) -- two chars, two DISTINCT
        // keys: stdlib §5's duplicate-key `ArgumentException` is a sibling
        // rule, not this one, so a constant key here would trip it instead.
        "    using var d3 := \"ab\".ToDictionary(lambda(c: char): string => Demo.Label(c)); "
        "    if d3.Count <> 2 then return 12; end if; "
        // GroupBy, both arms (T2's and the lazy variant)
        "    var n := a.GroupBy(lambda(x: int): int => Demo.Twice(x)).Count(); "
        "    if n <> 1 then return 13; end if; "
        "    var n2 := a.Where(lambda(x: int): bool => x > 0)"
        "                     .GroupBy(lambda(x: int): int => Demo.Twice(x)).Count(); "
        "    if n2 <> 1 then return 14; end if; "
        "    return 0; "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return Demo.Run(); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
}

// ── T7 — NEAR-MISS CONTROL, second shape: an IDENTITY key selector, whose
// annotated return type coincides with the source element type. MEASURED on
// these arms (not assumed): the gate reads the SELECTOR's return type, never
// the element type. A non-auto-hashable `Array<T>` element with an
// `int`-returning selector is ADMITTED; a hashable element with a
// non-hashable-returning selector is REJECTED. So the `keyTy = srcElem`
// default the arms carry is NOT taken here — an annotation always populates
// `funcReturn` — and the only thing separating T7 from T2 is that T2's
// annotated return type is itself a function type.
BEZEL_TEST(sema_linq_key_hashable_tkey_5959,
           identity_key_selector_still_accepted_control) {
    auto r = runAot(
        "module M5959H; "
        "static class Demo "
        "  public static method Run(): int "
        "    var a := new Array<int>(2); "
        "    a.Add(1); "
        "    using var d := a.ToDictionary(lambda(x: int): int => x); "
        "    if d.Count <> 1 then return 20; end if; "
        "    var n := a.GroupBy(lambda(x: int): int => x).Count(); "
        "    if n <> 1 then return 21; end if; "
        "    return 0; "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return Demo.Run(); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
}

// ── T8 — NEAR-MISS CONTROL, third shape: a FUNCTION-typed element type is
// still admitted through the members that do NOT key a hash container by
// it. `Select` producing a function-typed sequence and materializing it
// with ToArray is §27.7.1's "a collection of callbacks is an
// Array<(int) -> int>" remedy — the fix must not reach it.
BEZEL_TEST(sema_linq_key_hashable_tkey_5959,
           function_element_toarray_still_accepted_control_spec_27_7_1) {
    auto r = runAot(
        "module M5959I; "
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run(): int "
        "    var a := new Array<int>(2); "
        "    a.Add(1); "
        "    using var fs := a.Select(lambda(x: int): (int) -> int => Demo.Twice).ToArray(); "
        "    return 0; "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return Demo.Run(); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
}