// Codegen / soundness — the two-selector ToDictionary(keySelector, valueSelector)
// must type an INFERRED-type local (`using var d := ...`) as Dictionary<K, V>
// where V is the VALUE selector's return type — not the element T. Issue #1846.
// Spec: stdlib §5 (Materialization — `ToDictionary<TKey,TValue>(key: (T)->TKey,
// value: (T)->TValue): owned Dictionary<TKey, TValue>`).
//
// THE BUG (#1846). Sema's ToDictionary result-typing (ExprTypecheck_Calls.cpp)
// always built `Dictionary<K, T>` — using the source ELEMENT type T for V — and
// ignored the value selector. For `a.ToDictionary((s)=>s, (s)=>s.Length)` on an
// `Array<string>` this mistyped the inferred local `d` as
// `Dictionary<string, string>` instead of `Dictionary<string, int>`. The codegen
// MATERIALIZATION (LlvmBackend_Calls3) correctly stored the value selector's int
// result INLINE in the void* slot, but the codegen INDEXER read (`d["al"]`, keyed
// off `d`'s STATIC type) decoded the slot as a `string` — reinterpreting the
// inline int bits as a bz_string pointer and dereferencing them: a deterministic
// SIGSEGV on lookup (ASan: SEGV READ of a small-int-as-pointer). The int-key/
// int-value case escaped it only because there T == V == int, so the wrong V
// (element T) happened to equal the right V.
//
// THE FIX. Type the two-selector result's V as the value selector's return type
// (falling back to the element T only for the single-selector form). Now `d` is
// `Dictionary<string, int>`, so the indexer decodes the scalar int value inline —
// matching what the materialization stored.
//
// OBSERVABILITY. WITHOUT the fix the lookup SIGSEGVs, so runAotChecked's ASan net
// flips r.ran to false (the program crashed). WITH the fix the program runs,
// looks up present keys to their correct int values, and throws a clean
// KeyNotFoundException (not a SEGV) on an absent key. LSan-clean.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The exact #1846 repro: string key + a distinct scalar (int) value selector,
// with an INFERRED-type local (no `: Dictionary<...>` annotation — that would
// supply the type and bypass the mistyped inference). Present-key lookups must
// return the value selector's int results.
const char* kStringKeyIntValueInferred =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    using var a: Array<string> := [\"al\", \"bob\"]; "
    "    using var d := a.ToDictionary(lambda(s) => s, lambda(s) => s.Length); "
    "    Console.WriteLine(d[\"al\"].ToString()); "   // 2
    "    Console.WriteLine(d[\"bob\"].ToString()); "  // 3
    "    return d[\"al\"] + d[\"bob\"]; "             // 2 + 3 = 5
    "  end Main; "
    "end C;";

// Absent-key lookup on the (correctly-typed) result must raise a clean
// KeyNotFoundException (§4.8) — NOT a SIGSEGV from decoding an int as a string.
const char* kAbsentKeyThrowsCleanly =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    using var a: Array<string> := [\"al\", \"bob\"]; "
    "    using var d := a.ToDictionary(lambda(s) => s, lambda(s) => s.Length); "
    "    try "
    "      Console.WriteLine(d[\"zz\"].ToString()); "
    "    catch "
    "      var e: KeyNotFoundException => Console.WriteLine(\"knf\") "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// The other mistyped inference site: the LAZY-source pipeline
// (Where(...).ToDictionary(key, value)). Same string-key + int-value shape,
// inferred local. Pins the lazy path independently of the concrete-collection one.
const char* kLazySourceStringKeyIntValue =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    using var a: Array<string> := [\"al\", \"bob\", \"c\", \"dddd\"]; "
    "    using var d := "
    "        a.Where(lambda(s) => s.Length > 1).ToDictionary(lambda(s) => s, lambda(s) => s.Length); "
    "    Console.WriteLine(d[\"al\"].ToString()); "    // 2
    "    Console.WriteLine(d[\"dddd\"].ToString()); "  // 4
    "    return d[\"al\"] + d[\"dddd\"]; "             // 2 + 4 = 6
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #1846 — string key + int value selector, INFERRED result type: lookup returns
// the correct int values (no SIGSEGV). stdlib §5.
// =====================================================================
BEZEL_TEST(todict_two_selector_inferred_type_1846,
           string_key_int_value_lookup_spec_5) {
    JitResult r = runAotChecked(kStringKeyIntValueInferred);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);          // no SIGSEGV on lookup (ASan-clean)
    BEZEL_EXPECT_EQ(r.exitCode, 5);    // 2 + 3
    BEZEL_EXPECT_TRUE(r.stdoutText.find("2") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("3") != std::string::npos);
}

// =====================================================================
// #1846 — an absent key raises a clean KeyNotFoundException, not a SEGV. §4.8.
// =====================================================================
BEZEL_TEST(todict_two_selector_inferred_type_1846,
           absent_key_throws_knf_not_segv_spec_4_8) {
    JitResult r = runAotChecked(kAbsentKeyThrowsCleanly);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("knf") != std::string::npos);
}

// =====================================================================
// #1846 — the lazy-source inference site (Where(...).ToDictionary(k, v)) is
// fixed too. stdlib §5.
// =====================================================================
BEZEL_TEST(todict_two_selector_inferred_type_1846,
           lazy_source_string_key_int_value_spec_5) {
    JitResult r = runAotChecked(kLazySourceStringKeyIntValue);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);    // 2 + 4
}
