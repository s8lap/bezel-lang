// Spec-derived regression test for issue #2592.
//
// §7 (Nullability) / stdlib §4.8 (`Dictionary<K, V>`), v0.236: stdlib §4.8
// declares `Dictionary<K, V>.Add(key: K, value: V)` — TWO non-nullable slots
// of DIFFERENT types. #2582's builtin-collection §7 gate modelled a single
// element/key type (`genericArgs[0]`), so it checked only the KEY at pos 0;
// the VALUE at pos 1, typed `V` = `genericArgs[1]`, had no check at all. An
// un-narrowed `V?` was silently accepted and stored as the map's non-nullable
// `V` — the same §7 hole a builtin call with no MethodSymbol always has, on
// the one slot #2582 explicitly deferred (and filed as this issue).
//
// The check is against the INSTANTIATED `V`, which is what keeps a legitimate
// `Dictionary<string, int?>` — where the declared parameter IS `int?` —
// accepting an un-narrowed `int?`. That case is the one a naive "reject any
// nullable argument" gate breaks.
//
// This is a REJECTION fix, so each negative case asserts on diagnostic TEXT
// (not the coarse rejects() helper — runAot's WIFSIGNALED path synthesizes an
// Error diagnostic for a runtime SIGSEGV/ICE too, so it cannot distinguish a
// genuine reject from a crash). The positive cases guard against false
// rejection of working code.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

constexpr const char* kNullableDiag =
    "is nullable but the target type is non-nullable";

}  // namespace

// #2592 / §7 / stdlib §4.8: the issue's repro — `d.Add(k, v)` on
// `Dictionary<string, int>` with an un-narrowed `int?` VALUE. The key is a
// plain `string`, so the ONLY thing that can produce the diagnostic is the
// new pos-1 value check.
BEZEL_TEST(sema_dictionary_add_value_2592,
           dictionary_add_value_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", N()); "  // N(): int? — un-narrowed, into `value: V`
        "    return d.Count; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "Dictionary<T>.Add"));
    BEZEL_EXPECT_TRUE(hasError(r, "(\xC2\xA7""7)"));
}

// #2592 / stdlib §4.8: a class-typed `V` behaves the same — `Add(key: K,
// value: V)` declares a plain non-nullable `V` for class V too (the borrowing
// family does not wrap a class element in `V?`), so an un-narrowed `R?` is
// rejected.
BEZEL_TEST(sema_dictionary_add_value_2592,
           dictionary_add_class_value_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class R "
        "  public V: int; "
        "  public constructor(v: int) this.V := v; end constructor; "
        "end R; "
        "class C "
        "  private static method N(): R? => null; "
        "  public static method T(d: Dictionary<string, R>) "
        "    d.Add(\"a\", N()); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "Dictionary<T>.Add"));
}

// #2592 / §7: BOTH slots are checked independently — an un-narrowed nullable
// in the key position (the #2582 half) and in the value position (this fix)
// each produce their own diagnostic. Guards against the value check having
// displaced rather than joined the key check.
BEZEL_TEST(sema_dictionary_add_value_2592,
           dictionary_add_checks_key_and_value_slots_independently) {
    auto rKey = parseSemaAll(
        "module M; "
        "class C "
        "  private static method NK(): string? => null; "
        "  public static method T(d: Dictionary<string, int>) "
        "    d.Add(NK(), 1); "  // key slot only
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(rKey, kNullableDiag));

    auto rVal = parseSemaAll(
        "module M; "
        "class C "
        "  private static method NV(): int? => null; "
        "  public static method T(d: Dictionary<string, int>) "
        "    d.Add(\"a\", NV()); "  // value slot only
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(rVal, kNullableDiag));
}

// ---- Positive controls (guard against false rejection of working code) ----

// #2592 / §7 + §7.10: when `V` is ITSELF nullable — `Dictionary<string, int?>`
// declares `Add(key: string, value: int?)` — an un-narrowed `int?` value is
// admissible. This is THE case a naive "reject any nullable argument" gate
// breaks, and the reason the check runs against the instantiated `V` rather
// than a stripped non-nullable form.
BEZEL_TEST(sema_dictionary_add_value_2592,
           dictionary_add_accepts_nullable_value_type_instantiation) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(): int "
        "    var d := new Dictionary<string, int?>(); "
        "    d.Add(\"a\", N()); "  // Add(key: string, value: int?) — OK
        "    return d.Count; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2592 / §7: a plain non-nullable value compiles unchanged (positive
// baseline).
BEZEL_TEST(sema_dictionary_add_value_2592,
           dictionary_add_accepts_plain_value) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", 3); "
        "    return d.Count; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2592 / §7.4: a NARROWED `int?` value (proved non-null by a null-check) IS
// assignable to `value: V` — the narrowing exception lives inside
// checkAssignable and is reused unchanged.
BEZEL_TEST(sema_dictionary_add_value_2592,
           dictionary_add_accepts_narrowed_nullable_value) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var d := new Dictionary<string, int>(); "
        "    var n: int? := 4; "
        "    if n = null then return -1; end; "  // narrows n non-null below
        "    d.Add(\"a\", n); "
        "    return d.Count; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2592 / §22.3 + stdlib §4.9: `OwningDictionary<K, V>.Add(key: K, owned
// value: V)` marks the value `owned` — §22.3 governs that transfer, not §7.
// The value check is gated to `Dictionary` by name AND skipped by the
// modifier guard, so the owning form must not acquire a §7 diagnostic.
BEZEL_TEST(sema_dictionary_add_value_2592,
           owning_dictionary_add_owned_value_is_not_a_section7_slot) {
    auto r = parseSemaAll(
        "module M; "
        "class R "
        "  public V: int; "
        "  public constructor(v: int) this.V := v; end constructor; "
        "end R; "
        "class C "
        "  public static method T(d: OwningDictionary<string, R>) "
        "    d.Add(\"a\", owned new R(1)); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2592 / stdlib §4.10: `Set<T>.Add(item: T)` has ONE argument — the pos-1
// value rule must not mis-fire on a single-argument `Add` on another family
// (a position-classification guard).
BEZEL_TEST(sema_dictionary_add_value_2592,
           single_arg_add_on_other_family_is_unaffected) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(s: Set<int>) "
        "    s.Add(5); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}
