// Sema — fix #9262 / stdlib §4.9 + §5 (v0.359).
//
// THE DEFECT. stdlib §4.9's `OwningDictionary<K, V>` prose is explicit that
// its interface conformance is `IDisposable` only — "not
// `IOwningCollection<DictionaryEntry<K, V>>` … a borrowing entry view over an
// owning dictionary is a separate decision deliberately not taken here …
// Default `for var entry in ownedDict do` iteration is not provided on
// `OwningDictionary<K, V>`" — so NO member of stdlib §5's
// `public extension IEnumerable<T>` block has an applicable stdlib member on
// an `OwningDictionary<K, V>`: `OwningDictionary` conforms to neither
// `IEnumerable<T>` nor any of its supertypes.
//
// The compiler enforced that for the NON-NULLABLE tier (`First` / `Last` /
// `Single` / `ElementAt`, plus `ElementAtOrNull` from #8232) and NOT for the
// `*OrNull` tier: the §4.9 rejection list in
// `src/sema/ExprTypecheck_Calls.cpp` is hand-spelled by NAME and stopped at
// `ElementAtOrNull`, so `FirstOrNull` / `LastOrNull` / `SingleOrNull` — the
// `T?`-returning SIBLINGS of `First` / `Last` / `Single` on the SAME §5
// extension block — were not rejected in sema at all. They reached codegen and
// were refused THERE, with a codegen-internal wording naming an internal
// condition ("internal: `FirstOrNull` (stdlib §5, returns `T?`) was not typed
// `T?` on this receiver, so its result cannot be lowered soundly", #9234)
// instead of the stdlib §4.9 rule that decides the case.
//
// The verdict never changed (the call is refused either way) — the WORDING a
// user reads was the defect. These rows pin that the `*OrNull` tier now takes
// the SAME sema rejection and the SAME text as its non-nullable twins.
//
// Rows:
//
//   R1  the issue's own repro, verbatim — `od.FirstOrNull()`. RED on the
//       base (the base's wording is codegen-internal), GREEN here.
//   R2  `od.LastOrNull()`   — the sibling of R1.
//   R3  `od.SingleOrNull()` — the sibling of R1.
//   R4  `od.FirstOrNull(lambda(x) => …)` — the PREDICATE overload of the
//       tier (stdlib §5 declares both arities of each `*OrNull` name). RED
//       on the base (the base's wording there is codegen's #2457 "produced
//       no value, and no dispatch arm …"), GREEN here.
//
//   C1  (green control) `od.GetOrNull("a")` — a REAL §4.9 member — is still
//       admitted and still runs. Red under a wrong fix that rejects the
//       whole receiver instead of the §5 element-access tier.
//   C2  (green control) `od.First()` — the non-nullable twin — still takes
//       the SAME §4.9 text. Red under a wrong fix that rewrites the
//       diagnostic instead of extending the name list.

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
std::string orNullTierProgram(const char* member, const char* arg) {
    return "module P; import System; "
           "public class Box "
           "  public V: int := 1; "
           "end Box; "
           "public class Program "
           "  public static method Main() "
           "    using var od := new OwningDictionary<string, Box>(); "
           "    od[\"a\"] := new Box(); "
           "    var v := od." + std::string(member) + "(" + arg + "); "
           "    if v = null then Console.WriteLine(\"none\"); "
           "    else Console.WriteLine($\"first {v.Key}\"); end; "
           "  end Main; "
           "end Program;";
}

}  // namespace

// R1 — the issue's own repro. On the base this was refused by CODEGEN with
// "internal: `FirstOrNull` (stdlib §5, returns `T?`) was not typed `T?` on
// this receiver …"; here sema refuses it with the stdlib §4.9 rule.
BEZEL_TEST(sema_owning_dictionary_or_null_tier_9262,
           first_or_null_rejected_with_stdlib_4_9_text) {
    auto r = runAot(orNullTierProgram("FirstOrNull", ""));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`OwningDictionary<K, V>` does not expose `FirstOrNull`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §4.9"));
}

// R2 — the sibling name of the tier.
BEZEL_TEST(sema_owning_dictionary_or_null_tier_9262,
           last_or_null_rejected_with_stdlib_4_9_text) {
    auto r = runAot(orNullTierProgram("LastOrNull", ""));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`OwningDictionary<K, V>` does not expose `LastOrNull`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §4.9"));
}

// R3 — the sibling name of the tier.
BEZEL_TEST(sema_owning_dictionary_or_null_tier_9262,
           single_or_null_rejected_with_stdlib_4_9_text) {
    auto r = runAot(orNullTierProgram("SingleOrNull", ""));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`OwningDictionary<K, V>` does not expose `SingleOrNull`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §4.9"));
}

// R4 — the PREDICATE overload of the tier (stdlib §5 declares
// `FirstOrNull(predicate: (T) -> bool): T?` beside `FirstOrNull(): T?`). The
// base refused this one with codegen's #2457 "produced no value, and no
// dispatch arm emitted any code for this call" wording, which names an
// implementation gap rather than the stdlib rule that decides the case.
BEZEL_TEST(sema_owning_dictionary_or_null_tier_9262,
           first_or_null_predicate_rejected_with_stdlib_4_9_text) {
    auto r = runAot(orNullTierProgram("FirstOrNull",
                                      "lambda(x) => x.V = 1"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`OwningDictionary<K, V>` does not expose `FirstOrNull`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §4.9"));
}

// C1 — GREEN CONTROL. A REAL §4.9 member (`GetOrNull`, stdlib §4.9's
// borrowing look-don't-take surface) is still admitted on the same receiver
// and still runs. This row is RED under a wrong fix that rejects the
// receiver wholesale instead of the §5 element-access tier.
BEZEL_TEST(sema_owning_dictionary_or_null_tier_9262,
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

// C2 — GREEN CONTROL. The NON-NULLABLE twin keeps the SAME §4.9 text: this
// fix EXTENDS the name list, it does not rewrite the diagnostic. This row is
// RED under a wrong fix that rewords the message.
BEZEL_TEST(sema_owning_dictionary_or_null_tier_9262,
           first_twin_keeps_the_same_stdlib_4_9_text) {
    auto r = runAot(orNullTierProgram("First", ""));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`OwningDictionary<K, V>` does not expose `First`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §4.9"));
}
