// Sema-side regression tests for issue #8169 — the ENTRY-SHAPED
// `ICollection<DictionaryEntry<K, V>>` members of stdlib §4.8.
//
// stdlib §4.8 (v0.355) declares:
//
//   public class Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>
//
// so the §4.1 `ICollection<T>` surface (`Add(item)`, `Contains(item)`,
// `Remove(item)`) is reachable on the CONCRETE dictionary with a
// `DictionaryEntry<K, V>` argument, and §4.8 states their behaviour in one
// sentence:
//
//   "`Add(entry)` is `Add(entry.Key, entry.Value)` — ArgumentException on a
//    present key; `Contains(entry)` is `true` iff the key is present and the
//    stored value equals `entry.Value` under the type's standard equality;
//    `Remove(entry)` removes only when both match, else returns `false` with
//    no change."
//
// THIS FILE covers the SEMANTIC half only — what the front end admits and
// what it refuses:
//
//   * `Dictionary<K, V>` carries the entry forms: `Add(entry)`,
//     `Contains(entry)` and `Remove(entry)` are ADMITTED (no diagnostic).
//   * `OwningDictionary<K, V>` (§4.9) conforms to `IDisposable` ONLY — it
//     declares no `ICollection` conformance, hence no entry form — so
//     `Add(entry)` is REFUSED by the §22.6 arity table.
//   * `SortedDictionary<K, V>` (§4.17) conforms to `IReadOnlyCollection`
//     (read-only: no `Add`, no mutating `Remove`), so its entry form is
//     REFUSED by the same arity table.
//
// The lowering half (the `Add(entry)` / `Contains(entry)` / `Remove(entry)`
// arms themselves, the duplicate-key `ArgumentException`, the value equality
// and the ASan-clean ownership rows) lives in
// tests/codegen/dictionary_add_entry_8169_test.cpp.
//
// The §5.1 CHECK here is the front end's: it asks whether the ARITY of the
// call is one §4.8 (or §4.9 / §4.17) declares. §4.8's `Add` has BOTH a
// keyed two-argument form and the entry-shaped one-argument form, so the
// one-argument `Add` must NOT be refused by arity on a `Dictionary` receiver
// — that refusal is what turned the entry form into the #2457 "no dispatch
// arm" reject.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>
#include <string_view>

namespace {

// The gated front-end pipeline: lex + parse + collectSymbols + resolveTypes
// + typecheckExpressions, with the SAME staging gate the real driver uses
// (a parse error stops the later stages — support/sema_support.hpp).
bezel::test::SemaResult semaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
}

bool hasError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) return true;
    return false;
}

bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(sub) != std::string::npos)
            return true;
    return false;
}

// The §22.6 arity refusal's stable middle: "supplies 1 argument(s) but
// stdlib §4.9 declares `Add` with exactly 2 argument(s)".
constexpr const char* kOneArg = "supplies 1 argument(s)";
constexpr const char* kTwoArgAdd = "`Add` with exactly 2 argument(s)";

// A `DictionaryEntry<K, V>` bound to a local, so the entry-shaped call has a
// well-typed argument to pass.
constexpr const char* kEntryDecl =
    "    var e := new DictionaryEntry<string, int>(\"a\", 1); ";

}  // namespace

// ── §4.8: the entry forms are ADMITTED on `Dictionary<K, V>` ─────────────

// stdlib §4.8 (v0.355): `Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>`
// — the entry-shaped `Add(item)` is part of the §4.1 `ICollection<T>` surface
// the conformance declares, so `d.Add(entry)` is a well-typed call and the
// front end must not refuse it.
BEZEL_TEST(sema_dictionary_add_entry_8169,
           dictionary_add_entry_is_admitted_stdlib_4_8) {
    auto r = semaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var d := new Dictionary<string, int>(); "
        + std::string(kEntryDecl) +
        "    d.Add(e); "
        "    return d.Count; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r));
}

// The same §4.1 surface, `Contains(item)`: §4.8 declares
// "`Contains(entry)` is `true` iff the key is present and the stored value
// equals `entry.Value`" — a `bool`-returning member, so the call's RESULT
// type must resolve to `bool` too (an untyped call would make
// `var b := d.Contains(e)` infer nothing).
BEZEL_TEST(sema_dictionary_add_entry_8169,
           dictionary_contains_entry_is_admitted_stdlib_4_8) {
    auto r = semaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var d := new Dictionary<string, int>(); "
        + std::string(kEntryDecl) +
        "    var b := d.Contains(e); "
        "    if b then return 1; end; "
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r));
}

// And `Remove(item)`: "`Remove(entry)` removes only when both match, else
// returns `false` with no change" — also `bool`-returning.
BEZEL_TEST(sema_dictionary_add_entry_8169,
           dictionary_remove_entry_is_admitted_stdlib_4_8) {
    auto r = semaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var d := new Dictionary<string, int>(); "
        + std::string(kEntryDecl) +
        "    var b := d.Remove(e); "
        "    if b then return 1; end; "
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r));
}

// ── §4.9 / §4.17: the entry forms are NOT declared there ───────────────

// stdlib §4.9 (v0.355): `OwningDictionary<K, V>` conforms to `IDisposable`
// ONLY — it declares NO `ICollection` conformance, so the §4.1 entry-shaped
// `Add(item)` is not part of its surface, and §4.9's own `Add(key: K, owned
// value: V)` is the only `Add` it declares (TWO arguments). The one-argument
// call is therefore refused by the §22.6 arity table, citing §4.9.
//
// NOTE: `V` is a CLASS here, because §4.2/§14.4 forbid a primitive element
// type in an owning collection — an unrelated refusal that would otherwise
// add a second diagnostic to this row.
BEZEL_TEST(sema_dictionary_add_entry_8169,
           owning_dictionary_add_entry_is_refused_stdlib_4_9) {
    auto r = semaAll(
        "module M; "
        "class R "
        "  public V: int; "
        "  public constructor(v: int) this.V := v; end constructor; "
        "end R; "
        "class C "
        "  public static method T(): int "
        "    var d := new OwningDictionary<string, R>(); "
        "    var e := new DictionaryEntry<string, R>(\"a\", new R(1)); "
        "    d.Add(e); "
        "    return d.Count; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kOneArg));
    BEZEL_EXPECT_TRUE(hasError(r, kTwoArgAdd));
    // The refusal must cite §4.9 — the section that declares
    // `OwningDictionary.Add` — not §4.8, whose entry form belongs to
    // `Dictionary` alone.
    BEZEL_EXPECT_TRUE(hasError(r, "\xC2\xA7""4.9"));
}

// stdlib §4.17 (v0.355): `SortedDictionary<K, V>` conforms to
// `IReadOnlyCollection<DictionaryEntry<K, V>>` — a READ-ONLY conformance, so
// it declares no mutating entry form; its `Add(key: K, value: V)` (restored
// at v0.282 by proposal 0119) takes TWO arguments. The one-argument call is
// refused by the §22.6 arity table, citing §4.17.
BEZEL_TEST(sema_dictionary_add_entry_8169,
           sorted_dictionary_add_entry_is_refused_stdlib_4_17) {
    auto r = semaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var d := new SortedDictionary<string, int>(); "
        + std::string(kEntryDecl) +
        "    d.Add(e); "
        "    return d.Count; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kOneArg));
    BEZEL_EXPECT_TRUE(hasError(r, kTwoArgAdd));
    // §4.17, NOT §4.8: the sorted map's surface is its own.
    BEZEL_EXPECT_TRUE(hasError(r, "\xC2\xA7""4.17"));
}

// §4.17's `SortedDictionary<K, V>` declares NO `Add` at arity 1 — and its
// `Contains` / `Remove` are the KEYED members (`Contains(key: K)` /
// `Remove(key: K)`), which §4.17 does declare. The one-argument `Add` is
// therefore the only §4.8-shaped spelling the sorted map refuses by arity;
// this row pins that the refusal is SPECIFIC to `Add` (the keyed
// `Remove(key: K)` is still admitted — §4.17's own member).
BEZEL_TEST(sema_dictionary_add_entry_8169,
           sorted_dictionary_keyed_remove_stays_admitted_stdlib_4_17) {
    auto r = semaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var d := new SortedDictionary<string, int>(); "
        "    var b := d.Remove(\"a\"); "
        "    if b then return 1; end; "
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r));
}

// ── Controls ───────────────────────────────────────────────────────────

// The KEYED two-argument form §4.8 declares (`Add(key: K, value: V)`) is
// untouched by the entry form's admission — the guard is that the front end
// still accepts it, and still type-checks the value slot.
BEZEL_TEST(sema_dictionary_add_entry_8169,
           dictionary_keyed_add_is_still_admitted_stdlib_4_8) {
    auto r = semaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", 1); "
        "    return d.Count; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r));
}

// And the keyed one-argument forms — `ContainsKey(key: K)` /
// `Contains(key: K)` / `Remove(key: K)` — remain admissible, so admitting
// the ENTRY-shaped one-argument calls did not widen the keyed surface.
BEZEL_TEST(sema_dictionary_add_entry_8169,
           dictionary_keyed_one_arg_forms_are_still_admitted_stdlib_4_8) {
    auto r = semaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var d := new Dictionary<string, int>(); "
        "    var a := d.ContainsKey(\"a\"); "
        "    var b := d.Contains(\"a\"); "
        "    var c := d.Remove(\"a\"); "
        "    if a and b and c then return 1; end; "
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r));
}
