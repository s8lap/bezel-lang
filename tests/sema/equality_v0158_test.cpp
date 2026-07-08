// Spec-derived sema tests for the v0.158 record-equality revision (#1326 #1338).
//
// Authorities:
//  - §9.2 (v0.158) "Auto-generated equality" — the admissible-component-type
//    rule (a primitive/enum/`string`; another auto-equality-bearing record;
//    `T?` where T is admissible; a non-owning inline array `[N]T`/`[N]T?` with
//    admissible element) and the ToString narrowing (an `override ToString`
//    alone no longer disqualifies auto-equality; `override Equals`/`GetHashCode`
//    still do).
//  - §25.2 (v0.158, enum spelling corrected v0.183 #1381/#1582) co-declaration
//    rule — a record/enum overloading `=`/`<>` with both operands the
//    declaring type must also declare `Equals(T)` and — on a record,
//    `override GetHashCode`; on an enum, a plain `GetHashCode` (§10.5: enum
//    members never carry `override`).
//  - §3.6 / §8.3 (v0.158) equality-bearing — a `Dictionary<K,V>` key and a
//    `Set<T>` element must be equality-bearing; enforced at the instantiation
//    site (§3.6 rule 2).

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSema(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// The §9.2 use-site error on a non-auto-equality-bearing record.
bool hasNotAutoEqError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("auto-equality-bearing") != std::string::npos)
            return true;
    return false;
}

// The §25.2 co-declaration error.
bool hasCoDeclError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("must also declare `Equals(T)`") != std::string::npos)
            return true;
    return false;
}

// The §3.6/§8.3 equality-bearing instantiation error.
bool hasEqualityBearingError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("is not equality-bearing") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ─── §9.2 v0.158: admissible component types ────────────────────────────────

// §9.2: `T?` where T is admissible (`int?`) is an admissible component — the
// body record is auto-equality-bearing, so `=` is accepted.
BEZEL_TEST(equality_v0158, nullable_primitive_component_accepted_spec_9_2) {
    auto r = parseSema(
        "module M; "
        "record R "
        "  public property V: int?; "
        "  public constructor(v: int?) V := v; end constructor; "
        "end R; "
        "class C public method M(a: R, b: R) _ := a = b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.2: a nullable string component (`string?`) is admissible.
BEZEL_TEST(equality_v0158, nullable_string_component_accepted_spec_9_2) {
    auto r = parseSema(
        "module M; "
        "record R "
        "  public property Name: string?; "
        "  public constructor(n: string?) Name := n; end constructor; "
        "end R; "
        "class C public method M(a: R, b: R) _ := a.Equals(b); end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.2 / §8.3: a non-owning inline array `[N]T` is an admissible component.
BEZEL_TEST(equality_v0158, inline_array_component_accepted_spec_9_2) {
    auto r = parseSema(
        "module M; "
        "record R "
        "  public property A: [3]int; "
        "  public constructor(a: [3]int) A := a; end constructor; "
        "end R; "
        "class C public method M(a: R, b: R) _ := a = b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.2 / §8.3: `[N]T?` (nullable element) is admissible when T is admissible.
BEZEL_TEST(equality_v0158, inline_array_nullable_element_accepted_spec_9_2) {
    auto r = parseSema(
        "module M; "
        "record R "
        "  public property A: [3]int?; "
        "  public constructor(a: [3]int?) A := a; end constructor; "
        "end R; "
        "class C public method M(a: R, b: R) _ := a.GetHashCode(); end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.2: another auto-equality-bearing record is an admissible component
// (recursive rule).
BEZEL_TEST(equality_v0158, nested_auto_eq_record_component_accepted_spec_9_2) {
    auto r = parseSema(
        "module M; "
        "record Inner "
        "  public property X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "end Inner; "
        "record Outer "
        "  public property I: Inner; "
        "  public constructor(i: Inner) I := i; end constructor; "
        "end Outer; "
        "class C public method M(a: Outer, b: Outer) _ := a = b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.2: a class-typed auto-property is NOT an admissible component (a reference
// type) — the record is not auto-equality-bearing, so `=` is a compile error.
BEZEL_TEST(equality_v0158, class_component_rejected_spec_9_2) {
    auto r = parseSema(
        "module M; "
        "class K public v: int := 0; end K; "
        "record R "
        "  public property Ref: K; "
        "  public constructor(k: K) Ref := k; end constructor; "
        "end R; "
        "class C public method M(a: R, b: R) _ := a = b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNotAutoEqError(r));
}

// §9.2: an `Object`-typed auto-property is not admissible (a reference type).
BEZEL_TEST(equality_v0158, object_component_rejected_spec_9_2) {
    auto r = parseSema(
        "module M; "
        "record R "
        "  public property O: Object; "
        "  public constructor(o: Object) O := o; end constructor; "
        "end R; "
        "class C public method M(a: R, b: R) _ := a.Equals(b); end M; end C;");
    BEZEL_EXPECT_TRUE(hasNotAutoEqError(r));
}

// §9.2: a non-auto-equality-bearing record component makes the OUTER record
// ineligible too (the recursive rule fails for the inner record because it has
// an explicit field).
BEZEL_TEST(equality_v0158, nested_non_eq_record_component_rejected_spec_9_2) {
    auto r = parseSema(
        "module M; "
        "record Inner "
        "  public property X: int; "
        "  private _cache: int := 0; "
        "  public constructor(x: int) X := x; end constructor; "
        "end Inner; "
        "record Outer "
        "  public property I: Inner; "
        "  public constructor(i: Inner) I := i; end constructor; "
        "end Outer; "
        "class C public method M(a: Outer, b: Outer) _ := a = b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNotAutoEqError(r));
}

// ─── §9.2 v0.158: ToString narrowing (was #1249-A) ──────────────────────────

// §9.2 v0.158: an `override ToString` ALONE no longer disqualifies auto-equality
// — the record keeps auto-`Equals`/`GetHashCode`/`=`. So `=`, `.Equals`, and
// `.GetHashCode` are all accepted; only auto-`ToString` is suppressed (ToString
// dispatches to the override).
BEZEL_TEST(equality_v0158, tostring_override_keeps_auto_equality_spec_9_2) {
    auto r = parseSema(
        "module M; "
        "record R "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public override method ToString(): string => \"R!\"; "
        "end R; "
        "class C public method M(a: R, b: R) "
        "  _ := a = b; _ := a.Equals(b); _ := a.GetHashCode(); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.2 v0.158 (control): an `override Equals`/`GetHashCode` still fully
// disqualifies auto-equality — `=` is a compile error (unless the author also
// declares `operator =`/`<>`).
BEZEL_TEST(equality_v0158, equals_override_still_disqualifies_spec_9_2) {
    auto r = parseSema(
        "module M; "
        "record R "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public override method Equals(o: Object?): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end R; "
        "class C public method M(a: R, b: R) _ := a = b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNotAutoEqError(r));
}

// ─── §25.2 v0.158: co-declaration rule (#1338) ──────────────────────────────

// §25.2: a record overloading `=`/`<>` (homogeneous operands) WITHOUT a
// co-declared `Equals(T)` + `override GetHashCode` is a compile error.
BEZEL_TEST(equality_v0158, codecl_missing_pair_rejected_spec_25_2) {
    auto r = parseSema(
        "module M; "
        "record V(n: int) "
        "  public static method operator =(a: V, b: V): bool => a.n = b.n; "
        "  public static method operator <>(a: V, b: V): bool => a.n <> b.n; "
        "end V; class F public static method Main(): int return 0; end Main; end F;");
    BEZEL_EXPECT_TRUE(hasCoDeclError(r));
}

// §25.2: the same record WITH the co-declared members is accepted (no co-decl
// error).
BEZEL_TEST(equality_v0158, codecl_satisfied_accepted_spec_25_2) {
    auto r = parseSema(
        "module M; "
        "record V(n: int) "
        "  public static method operator =(a: V, b: V): bool => a.n = b.n; "
        "  public static method operator <>(a: V, b: V): bool => a.n <> b.n; "
        "  public method Equals(other: V): bool => n = other.n; "
        "  public override method GetHashCode(): int => n; "
        "end V; class F public static method Main(): int return 0; end Main; end F;");
    BEZEL_EXPECT_FALSE(hasCoDeclError(r));
}

// §25.2: a HETEROGENEOUS-operand overload (`V = int`) carries no co-declaration
// obligation — no error even without `Equals(T)`/`GetHashCode`.
BEZEL_TEST(equality_v0158, codecl_heterogeneous_exempt_spec_25_2) {
    auto r = parseSema(
        "module M; "
        "record V(n: int) "
        "  public static method operator =(a: V, b: int): bool => a.n = b; "
        "  public static method operator <>(a: V, b: int): bool => a.n <> b; "
        "end V; class F public static method Main(): int return 0; end Main; end F;");
    BEZEL_EXPECT_FALSE(hasCoDeclError(r));
}

// §25.2: an ENUM overloading `=`/`<>` (homogeneous) is subject to the same rule.
BEZEL_TEST(equality_v0158, codecl_enum_missing_pair_rejected_spec_25_2) {
    auto r = parseSema(
        "module M; "
        "enum Color Red, Green; "
        "  public static method operator =(a: Color, b: Color): bool => true; "
        "  public static method operator <>(a: Color, b: Color): bool => false; "
        "end Color; class F public static method Main(): int return 0; end Main; end F;");
    BEZEL_EXPECT_TRUE(hasCoDeclError(r));
}

// §25.2 (v0.183, #1381/#1582): on an enum, the co-declaration diagnostic must
// instruct a PLAIN `GetHashCode`, not `override GetHashCode` — §10.5 rejects
// `override` on any enum method (enums dispatch statically, no override
// chain), so telling an enum author to write `override GetHashCode` sends
// them straight into a second, unrelated compile error. Before the v0.183 fix
// this message hardcoded "override GetHashCode" for every `t->kind`.
BEZEL_TEST(equality_v0158,
           codecl_enum_message_says_plain_gethashcode_spec_25_2_v0_183) {
    auto r = parseSema(
        "module M; "
        "enum Color Red, Green; "
        "  public static method operator =(a: Color, b: Color): bool => true; "
        "  public static method operator <>(a: Color, b: Color): bool => false; "
        "end Color; class F public static method Main(): int return 0; end Main; end F;");
    bool sawPlainSpelling = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("must also declare `Equals(T)`") == std::string::npos)
            continue;
        // Must name the enum's own spelling ...
        sawPlainSpelling = d.message.find("`GetHashCode`") != std::string::npos;
        // ... and must NOT tell the author to write `override GetHashCode`,
        // which §10.5 would then reject.
        BEZEL_EXPECT_TRUE(
            d.message.find("override GetHashCode") == std::string::npos);
    }
    BEZEL_EXPECT_TRUE(sawPlainSpelling);
}

// §25.2 (v0.183): the record spelling is unchanged — still `override
// GetHashCode` — so a record author is not affected by the enum-specific fix.
BEZEL_TEST(equality_v0158,
           codecl_record_message_still_says_override_gethashcode_spec_25_2) {
    auto r = parseSema(
        "module M; "
        "record V(n: int) "
        "  public static method operator =(a: V, b: V): bool => a.n = b.n; "
        "  public static method operator <>(a: V, b: V): bool => a.n <> b.n; "
        "end V; class F public static method Main(): int return 0; end Main; end F;");
    bool sawOverrideSpelling = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("must also declare `Equals(T)`") == std::string::npos)
            continue;
        sawOverrideSpelling =
            d.message.find("`override GetHashCode`") != std::string::npos;
    }
    BEZEL_EXPECT_TRUE(sawOverrideSpelling);
}

// §25.2 / §10.5: an enum WITH the co-declared members — a plain (non-`override`)
// `Equals(T)` + `GetHashCode` — satisfies the rule; the enum's own spelling
// (§10.5: enum members never carry `override`) is exactly what the compiler
// accepts, not just what it now recommends.
BEZEL_TEST(equality_v0158, codecl_enum_satisfied_with_plain_gethashcode_spec_25_2) {
    auto r = parseSema(
        "module M; "
        "enum Color Red, Green; "
        "  public static method operator =(a: Color, b: Color): bool => int(a) = int(b); "
        "  public static method operator <>(a: Color, b: Color): bool => int(a) <> int(b); "
        "  public method Equals(other: Color): bool => int(this) = int(other); "
        "  public method GetHashCode(): int => int(this); "
        "end Color; class F public static method Main(): int return 0; end Main; end F;");
    BEZEL_EXPECT_FALSE(hasCoDeclError(r));
}

// ─── §3.6 / §8.3 v0.158: equality-bearing hash-container keys (#1338) ────────

// §3.6/§8.3: an `int` key is equality-bearing — `Dictionary<int, int>` accepted.
BEZEL_TEST(equality_v0158, dict_int_key_accepted_spec_3_6) {
    auto r = parseSema(
        "module M; class C public method M(d: Dictionary<int, int>) end M; end C;");
    BEZEL_EXPECT_FALSE(hasEqualityBearingError(r));
}

// §3.6/§8.3: a `string` key is equality-bearing.
BEZEL_TEST(equality_v0158, dict_string_key_accepted_spec_3_6) {
    auto r = parseSema(
        "module M; class C public method M(d: Dictionary<string, int>) end M; end C;");
    BEZEL_EXPECT_FALSE(hasEqualityBearingError(r));
}

// §3.6/§8.3: an enum key is equality-bearing.
BEZEL_TEST(equality_v0158, dict_enum_key_accepted_spec_3_6) {
    auto r = parseSema(
        "module M; enum Color Red, Green; end Color; "
        "class C public method M(d: Dictionary<Color, int>) end M; end C;");
    BEZEL_EXPECT_FALSE(hasEqualityBearingError(r));
}

// §3.6/§8.3: a class key is equality-bearing (reference identity is a consistent
// Equals/GetHashCode).
BEZEL_TEST(equality_v0158, dict_class_key_accepted_spec_3_6) {
    auto r = parseSema(
        "module M; class K public v: int := 0; end K; "
        "class C public method M(d: Dictionary<K, int>) end M; end C;");
    BEZEL_EXPECT_FALSE(hasEqualityBearingError(r));
}

// §3.6/§8.3: a positional record key (always synthesizes equality, §9.1) is
// equality-bearing.
BEZEL_TEST(equality_v0158, dict_positional_record_key_accepted_spec_3_6) {
    auto r = parseSema(
        "module M; record Point(x: int, y: int); "
        "class C public method M(d: Dictionary<Point, int>) end M; end C;");
    BEZEL_EXPECT_FALSE(hasEqualityBearingError(r));
}

// §3.6/§8.3: an auto-equality-bearing body record key is equality-bearing.
BEZEL_TEST(equality_v0158, dict_autoeq_body_record_key_accepted_spec_3_6) {
    auto r = parseSema(
        "module M; "
        "record Key "
        "  public property Id: int; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Key; "
        "class C public method M(d: Dictionary<Key, int>) end M; end C;");
    BEZEL_EXPECT_FALSE(hasEqualityBearingError(r));
}

// §3.6/§8.3: a body record that is neither auto-equality-bearing (it has an
// explicit field) nor declares user equality is NOT equality-bearing — a
// `Dictionary<K, V>` key of that type is rejected at the instantiation site.
BEZEL_TEST(equality_v0158, dict_non_eq_record_key_rejected_spec_3_6) {
    auto r = parseSema(
        "module M; "
        "record Bad "
        "  public property Id: int; "
        "  private _cache: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Bad; "
        "class C public method M(d: Dictionary<Bad, int>) end M; end C;");
    BEZEL_EXPECT_TRUE(hasEqualityBearingError(r));
}

// §3.6/§8.3: a `Set<T>` element is subject to the same rule — a non-equality-
// bearing record element is rejected.
BEZEL_TEST(equality_v0158, set_non_eq_record_element_rejected_spec_3_6) {
    auto r = parseSema(
        "module M; "
        "record Bad "
        "  public property Id: int; "
        "  private _cache: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Bad; "
        "class C public method M(s: Set<Bad>) end M; end C;");
    BEZEL_EXPECT_TRUE(hasEqualityBearingError(r));
}

// §3.6/§8.3: an `int` `Set<T>` element is fine (no over-reject on primitives).
BEZEL_TEST(equality_v0158, set_int_element_accepted_spec_3_6) {
    auto r = parseSema(
        "module M; class C public method M(s: Set<int>) end M; end C;");
    BEZEL_EXPECT_FALSE(hasEqualityBearingError(r));
}

// §3.6/§8.3: a record that overloads `=`/`<>` (with co-declared members) DOES
// declare user equality — it is equality-bearing and accepted as a Set element.
BEZEL_TEST(equality_v0158, set_user_equality_record_element_accepted_spec_3_6) {
    auto r = parseSema(
        "module M; "
        "record V(n: int) "
        "  public static method operator =(a: V, b: V): bool => a.n = b.n; "
        "  public static method operator <>(a: V, b: V): bool => a.n <> b.n; "
        "  public method Equals(other: V): bool => n = other.n; "
        "  public override method GetHashCode(): int => n; "
        "end V; "
        "class C public method M(s: Set<V>) end M; end C;");
    BEZEL_EXPECT_FALSE(hasEqualityBearingError(r));
}

// ─── §9.3 v0.158: positional/combined-record admissibility (#1383) ───────────

// §9.3: a purely-positional record whose positional field is a class type (not
// admissible per §9.2) gets the same use-site-error discipline as a body record
// with a non-admissible component — `=` is a compile error (not silent
// reference-shaped equality).
BEZEL_TEST(equality_v0158, positional_class_field_eq_rejected_spec_9_3) {
    auto r = parseSema(
        "module M; "
        "class K public v: int := 0; end K; "
        "record BadPos(Ref: K); "
        "class C public method M(a: BadPos, b: BadPos) _ := a = b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNotAutoEqError(r));
}

// §9.3: a purely-positional record with admissible fields (int, string) keeps
// its auto-generated equality — `=` is accepted.
BEZEL_TEST(equality_v0158, positional_admissible_fields_eq_accepted_spec_9_3) {
    auto r = parseSema(
        "module M; "
        "record GoodPos(X: int, Y: string); "
        "class C public method M(a: GoodPos, b: GoodPos) _ := a = b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.3: `.Equals` on a positional record with a class-typed field is also a
// use-site error (the same as `=`).
BEZEL_TEST(equality_v0158, positional_class_field_equals_call_rejected_spec_9_3) {
    auto r = parseSema(
        "module M; "
        "class K public v: int := 0; end K; "
        "record BadPos(Ref: K); "
        "class C public method M(a: BadPos, b: BadPos) _ := a.Equals(b); end M; end C;");
    BEZEL_EXPECT_TRUE(hasNotAutoEqError(r));
}

// §9.3: a combined-form record (positional fields + body) with a class-typed
// positional field is not auto-equality-bearing — `=` is a compile error.
BEZEL_TEST(equality_v0158, combined_class_positional_field_rejected_spec_9_3) {
    auto r = parseSema(
        "module M; "
        "class K public v: int := 0; end K; "
        "record Combined(Ref: K) "
        "  public method Len(): int => 0; "
        "end Combined; "
        "class C public method M(a: Combined, b: Combined) _ := a = b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNotAutoEqError(r));
}

// §9.3: a combined-form record with only admissible positional fields and a
// trivial body is auto-equality-bearing — `=` is accepted.
BEZEL_TEST(equality_v0158, combined_admissible_fields_eq_accepted_spec_9_3) {
    auto r = parseSema(
        "module M; "
        "record Vec(X: double, Y: double) "
        "  public method Len(): double => 0.0; "
        "end Vec; "
        "class C public method M(a: Vec, b: Vec) _ := a = b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.3: a positional record with a class-typed field is NOT equality-bearing,
// so it is rejected as a Dictionary key.
BEZEL_TEST(equality_v0158, positional_class_field_dict_key_rejected_spec_9_3) {
    auto r = parseSema(
        "module M; "
        "class K public v: int := 0; end K; "
        "record BadPos(Ref: K); "
        "class C public method M(d: Dictionary<BadPos, int>) end M; end C;");
    BEZEL_EXPECT_TRUE(hasEqualityBearingError(r));
}

// §9.3 / §3.6: a nested positional record in another record — if its positional
// field is a class (not admissible), the outer record is also not admissible.
BEZEL_TEST(equality_v0158, nested_positional_non_admissible_rejects_outer_spec_9_3) {
    auto r = parseSema(
        "module M; "
        "class K public v: int := 0; end K; "
        "record Inner(Ref: K); "   // not admissible (class field)
        "record Outer "
        "  public property I: Inner; "
        "  public constructor(i: Inner) I := i; end constructor; "
        "end Outer; "
        "class C public method M(a: Outer, b: Outer) _ := a = b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNotAutoEqError(r));
}

// ─── §3.6 / §8.3 v0.158: generic type param key — per-instantiation (#1384) ──

// §3.6 rule 2 / #1384: when a hash container is used inside a generic type
// body with a type parameter as the key, the site is CONSERVATIVELY accepted
// (no constraint surface for equality-bearing exists). The check fires
// per-instantiation: a concrete instantiation with a non-equality-bearing key
// is rejected at the instantiation site.
BEZEL_TEST(equality_v0158, dict_generic_param_key_concrete_bad_instantiation_spec_3_6) {
    auto r = parseSema(
        "module M; "
        // Inner non-equality-bearing body record
        "record Bad "
        "  public property Id: int; "
        "  private _cache: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Bad; "
        // Generic class whose M method uses Dictionary<K2, int>
        "class Outer<K2> "
        "  public method M(d: Dictionary<K2, int>) end M; "
        "end Outer; "
        // Concrete instantiation Outer<Bad> — the key becomes Bad at this site
        "class C public method Use(o: Outer<Bad>) end Use; end C;");
    BEZEL_EXPECT_TRUE(hasEqualityBearingError(r));
}
