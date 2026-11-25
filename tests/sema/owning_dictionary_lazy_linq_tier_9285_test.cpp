// Sema — fix #9285 / stdlib §4.9 + §5 (v0.359).
//
// THE DEFECT. stdlib §4.9's `OwningDictionary<K, V>` prose is explicit that
// its interface conformance is `IDisposable` only — "not
// `IOwningCollection<DictionaryEntry<K, V>>` … a borrowing entry view over an
// owning dictionary is a separate decision deliberately not taken here …
// Default `for var entry in ownedDict do` iteration is not provided on
// `OwningDictionary<K, V>`" — so NO member of stdlib §5's
// `public extension IEnumerable<T>` block has an applicable stdlib member on
// an `OwningDictionary<K, V>`: the receiver conforms to neither
// `IEnumerable<T>` nor any of its supertypes.
//
// The compiler enforced that for the EAGER element-access tier (`First` /
// `Last` / `Single` / `ElementAt` / `ElementAtOrNull`, plus the `*OrNull`
// tier from #9262) and NOT for §5's LAZY tier — the operators that build a
// DEFERRED sequence over the receiver (`Take` / `Skip` / `TakeWhile` /
// `SkipWhile` / `Where` / `Select` / `SelectMany` / `OrderBy` /
// `OrderByDescending`, plus §5's `where T: hashable` block's `Distinct`).
// Those calls were ADMITTED: the program COMPILED (exit 0), and only the
// CONSUMPTION of the result failed, at codegen ("not yet implemented: codegen
// for this for statement").
//
// The cause is the generic §5 LINQ exemption every concrete-collection head
// reads (`concreteCollectionCallSurfaceAdmits` →
// `isDocumentedStdlibLinqOperatorName`): it admits the WHOLE §5 name table
// for EVERY head, `OwningDictionary` included. The §4.9 rule — the one block
// that knows this receiver's conformance — is hand-spelled by NAME, so the
// LAZY tier had to be added to it (the shape #8232 and #9262 already used).
//
// Rows:
//
//   R1  the issue's own repro, verbatim — `od.Take(1)`. RED on the base (the
//       base COMPILES it), GREEN here.
//   R2  `od.Skip(1)`                 — the sibling of R1.
//   R3  `od.Distinct()`              — §5's `where T: hashable` block's name.
//   R4  `od.TakeWhile(lambda(x) => …)` — the callback-taking lazy sibling.
//   R5  `od.SkipWhile(lambda(x) => …)` — the sibling of R4.
//   R6  `od.Where(lambda(x) => …)`   — the filtering sibling.
//   R7  `od.Select(lambda(x) => …)`  — the projection sibling.
//   R8  `od.SelectMany(lambda(x) => …)` — the projection sibling.
//   R9  `od.OrderBy(lambda(x) => …)` — the ordering sibling.
//   R10 `od.OrderByDescending(lambda(x) => …)` — the sibling of R9.
//   R11 `od2?.Take(1)` — the `?.` SPELLING of R1. Language spec §7.5 rule 2
//       (v0.359) makes the two spellings of ONE member resolve identically,
//       and the `?.` call is typed by a SPECULATIVE `.` twin whose report is
//       discarded — so the `.` spelling's refusal was rolled back with it and
//       the `?.` spelling compiled with no diagnostic at all. RED on the base.
//
//   C1  (green control) `od.GetOrNull("a")` — a REAL §4.9 member — is still
//       admitted and still runs. Red under a wrong fix that rejects the
//       whole receiver instead of §5's operator tier.
//   C2  (green control) `od.First()` — the EAGER twin — still takes the SAME
//       §4.9 text. Red under a wrong fix that rewords the diagnostic instead
//       of extending the name list.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The issue's literal repro (module P; import System; …), with the member
// call under test swapped in. `Box` carries the value the dictionary owns;
// `od["a"] := new Box()` puts one entry in it.
std::string lazyTierProgram(const char* call) {
    return "module P; import System; "
           "public class Box "
           "  public V: int := 1; "
           "end Box; "
           "public class Program "
           "  public static method Main() "
           "    using var od := new OwningDictionary<string, Box>(); "
           "    od[\"a\"] := new Box(); "
           "    var v := od." + std::string(call) + "; "
           "    Console.WriteLine(\"compiled\"); "
           "  end Main; "
           "end Program;";
}

// The `?.` spelling of the same call: the receiver is a NULLABLE
// `OwningDictionary<string, Box>?` (a null receiver is what makes `?.` the
// spelling it is), and the call is `od2?.M(...)`.
std::string lazyTierNullCondProgram(const char* call) {
    return "module P; import System; "
           "public class Box "
           "  public V: int := 1; "
           "end Box; "
           "public class Program "
           "  public static method Main() "
           "    using var od := new OwningDictionary<string, Box>(); "
           "    od[\"a\"] := new Box(); "
           "    var od2: OwningDictionary<string, Box>? := null; "
           "    var v := od2?." + std::string(call) + "; "
           "    Console.WriteLine(\"compiled\"); "
           "  end Main; "
           "end Program;";
}

}  // namespace

// R1 — the issue's own repro. On the base this COMPILED (exit 0); here sema
// refuses it with the stdlib §4.9 rule.
BEZEL_TEST(sema_owning_dictionary_lazy_linq_tier_9285,
           take_rejected_with_stdlib_4_9_text) {
    auto r = runAot(lazyTierProgram("Take(1)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`OwningDictionary<K, V>` does not expose `Take`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §4.9"));
}

// R2 — the sibling name of the tier.
BEZEL_TEST(sema_owning_dictionary_lazy_linq_tier_9285,
           skip_rejected_with_stdlib_4_9_text) {
    auto r = runAot(lazyTierProgram("Skip(1)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`OwningDictionary<K, V>` does not expose `Skip`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §4.9"));
}

// R3 — §5's `where T: hashable` block's name (`Distinct(): IEnumerable<T>`).
BEZEL_TEST(sema_owning_dictionary_lazy_linq_tier_9285,
           distinct_rejected_with_stdlib_4_9_text) {
    auto r = runAot(lazyTierProgram("Distinct()"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`OwningDictionary<K, V>` does not expose `Distinct`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §4.9"));
}

// R4 — the callback-taking lazy sibling (stdlib §5 declares
// `TakeWhile(predicate: (T) -> bool): IEnumerable<T>`).
BEZEL_TEST(sema_owning_dictionary_lazy_linq_tier_9285,
           take_while_rejected_with_stdlib_4_9_text) {
    auto r = runAot(lazyTierProgram("TakeWhile(lambda(x) => true)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`OwningDictionary<K, V>` does not expose `TakeWhile`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §4.9"));
}

// R5 — the sibling of R4.
BEZEL_TEST(sema_owning_dictionary_lazy_linq_tier_9285,
           skip_while_rejected_with_stdlib_4_9_text) {
    auto r = runAot(lazyTierProgram("SkipWhile(lambda(x) => true)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`OwningDictionary<K, V>` does not expose `SkipWhile`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §4.9"));
}

// R6 — the filtering sibling.
BEZEL_TEST(sema_owning_dictionary_lazy_linq_tier_9285,
           where_rejected_with_stdlib_4_9_text) {
    auto r = runAot(lazyTierProgram("Where(lambda(x) => true)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`OwningDictionary<K, V>` does not expose `Where`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §4.9"));
}

// R7 — the projection sibling.
BEZEL_TEST(sema_owning_dictionary_lazy_linq_tier_9285,
           select_rejected_with_stdlib_4_9_text) {
    auto r = runAot(lazyTierProgram("Select(lambda(x) => 1)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`OwningDictionary<K, V>` does not expose `Select`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §4.9"));
}

// R8 — the projection sibling (stdlib §5 declares
// `SelectMany<U>(selector: (T) -> IEnumerable<U>): IEnumerable<U>`).
BEZEL_TEST(sema_owning_dictionary_lazy_linq_tier_9285,
           select_many_rejected_with_stdlib_4_9_text) {
    auto r = runAot(lazyTierProgram("SelectMany(lambda(x) => new Box())"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`OwningDictionary<K, V>` does not expose `SelectMany`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §4.9"));
}

// R9 — the ordering sibling (stdlib §5 declares
// `OrderBy<TKey>(key: (T) -> TKey): IOrderedEnumerable<T>`).
BEZEL_TEST(sema_owning_dictionary_lazy_linq_tier_9285,
           order_by_rejected_with_stdlib_4_9_text) {
    auto r = runAot(lazyTierProgram("OrderBy(lambda(x) => 1)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`OwningDictionary<K, V>` does not expose `OrderBy`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §4.9"));
}

// R10 — the sibling of R9.
BEZEL_TEST(sema_owning_dictionary_lazy_linq_tier_9285,
           order_by_descending_rejected_with_stdlib_4_9_text) {
    auto r = runAot(lazyTierProgram("OrderByDescending(lambda(x) => 1)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`OwningDictionary<K, V>` does not expose `OrderByDescending`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §4.9"));
}

// R11 — the `?.` SPELLING of R1. Language spec §7.5 rule 2 (v0.359):
// "`x?.M` resolves `M` as `x.M` does." The `?.` call is typed by a
// SPECULATIVE `.` twin whose diagnostics are discarded, so on the base the
// `.` spelling's refusal was rolled back with the twin's and this call
// compiled with NO diagnostic at all.
BEZEL_TEST(sema_owning_dictionary_lazy_linq_tier_9285,
           null_conditional_take_rejected_with_stdlib_4_9_text) {
    auto r = runAot(lazyTierNullCondProgram("Take(1)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`OwningDictionary<K, V>` does not expose `Take`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §4.9"));
}

// C1 — GREEN CONTROL. A REAL §4.9 member (`GetOrNull`, stdlib §4.9's
// borrowing look-don't-take surface) is still admitted on the same receiver
// and still runs. This row is RED under a wrong fix that rejects the
// receiver wholesale instead of §5's operator tier.
BEZEL_TEST(sema_owning_dictionary_lazy_linq_tier_9285,
           get_or_null_still_admitted_4_9_surface_unchanged) {
    auto r = runAot(
        "module P; import System; "
        "public class Box "
        "  public V: int := 1; "
        "end Box; "
        "public class Program "
        "  public static method Main() "
        "    using var od := new OwningDictionary<string, Box>(); "
        "    od[\"a\"] := new Box(); "
        "    var v := od.GetOrNull(\"a\"); "
        "    if v = null then Console.WriteLine(\"none\"); "
        "    else Console.WriteLine($\"v {v!.V}\"); end; "
        "    Console.WriteLine(\"ok\"); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "v 1\nok\n");
}

// C2 — GREEN CONTROL. The EAGER twin keeps the SAME §4.9 text: this fix
// EXTENDS the name list, it does not rewrite the diagnostic. This row is RED
// under a wrong fix that rewords the message.
BEZEL_TEST(sema_owning_dictionary_lazy_linq_tier_9285,
           first_twin_keeps_the_same_stdlib_4_9_text) {
    auto r = runAot(lazyTierProgram("First()"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`OwningDictionary<K, V>` does not expose `First`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §4.9"));
}
