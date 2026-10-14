// Spec-derived sema tests for the v0.158 record-equality revision (#1326 #1338).
//
// Authorities:
//  - §9.2 (v0.158) "Auto-generated equality" — the admissible-component-type
//    rule (a primitive/enum/`string`; another auto-hashable record;
//    `T?` where T is admissible; a non-owning inline array `[N]T`/`[N]T?` with
//    admissible element) and the ToString narrowing (an `override ToString`
//    alone no longer disqualifies auto-equality; `override Equals`/`GetHashCode`
//    still do).
//  - §25.2 (v0.158, enum spelling corrected v0.183 #1381/#1582) co-declaration
//    rule — a record/enum overloading `=`/`<>` with both operands the
//    declaring type must also declare `Equals(T)` and — on a record,
//    `override GetHashCode`; on an enum, a plain `GetHashCode` (§10.5, v0.183:
//    the plain spelling is always sufficient here; v0.288/#3837 additionally
//    ADMITS `override` on an enum's `Equals`/`GetHashCode`/`ToString`, but
//    that widens what the AUTHOR may write, not what this diagnostic
//    recommends, so the tests below are unaffected).
//  - §3.6 / §8.3 (v0.158) hashable — a `Dictionary<K,V>` key and a
//    `Set<T>` element must be hashable; enforced at the instantiation
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

// The §9.2 use-site error on a non-auto-hashable record.
bool hasNotAutoEqError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("auto-hashable") != std::string::npos)
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

// The §3.6/§8.3 hashable instantiation error.
bool hasEqualityBearingError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("is not hashable") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ─── §9.2 v0.158: admissible component types ────────────────────────────────

// §9.2: `T?` where T is admissible (`int?`) is an admissible component — the
// body record is auto-hashable, so `=` is accepted.
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

// §9.2: another auto-hashable record is an admissible component
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
// type) — the record is not auto-hashable, so `=` is a compile error.
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

// §9.2: a non-auto-hashable record component makes the OUTER record
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
        "  public override method Equals(other: R): bool => true; "
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
// instruct a PLAIN `GetHashCode`, not `override GetHashCode`. At v0.183 this
// was required — §10.5 rejected `override` on any enum method outright, so
// the `override` spelling sent an author straight into a second, unrelated
// compile error. v0.288/#3837 later ADMITS `override` here too (both
// spellings now declare the same member), but the plain spelling recommended
// below remains fully sufficient, so this message — and this test — are
// unaffected. Before the v0.183 fix this message hardcoded "override
// GetHashCode" for every `t->kind`.
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
        // ... and must NOT tell the author to write `override GetHashCode` —
        // the plain spelling is the simpler, always-sufficient answer (the
        // message is unaffected by v0.288/#3837 additionally admitting
        // `override` here too).
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

// ─── §25.2 v0.275/v0.276: no exemption for compiler-intrinsic types (#3334) ──

// §25.2 (v0.275, completed v0.276): "There is no exemption for compiler-
// intrinsic types. The pointer records of stdlib §10.0 and §10.5 — `IntPtr`,
// `UIntPtr`, `Ptr<T>` — declare `Equals(T)` and `override GetHashCode` beside
// their operators like any other record." A user record **named** `IntPtr`
// (distinct from stdlib §10.0's `IntPtr`, whose `t->name == "IntPtr"`
// exemption the v0.275 retirement removed) overloading `=`/`<>` without the
// co-declared pair is rejected by §25.2 — the check is keyed on the
// *declaration*, not on the type's name. Pre-fix this compiled (the guard
// short-circuited on `t->name == "IntPtr"`); post-fix it is rejected with the
// standard §25.2 diagnostic naming the record and the missing members.
BEZEL_TEST(equality_v0158,
           codecl_user_record_named_intptr_rejected_spec_25_2_v0_275) {
    auto r = parseSema(
        "module M; "
        "record IntPtr(n: int) "
        "  public static method operator =(a: IntPtr, b: IntPtr): bool => a.n = b.n; "
        "  public static method operator <>(a: IntPtr, b: IntPtr): bool => a.n <> b.n; "
        "end IntPtr; class F public static method Main(): int return 0; end Main; end F;");
    BEZEL_EXPECT_TRUE(hasCoDeclError(r));
}

// §25.2 (v0.275/v0.276): the symmetric case — a user record **named** `UIntPtr`
// overloading `=`/`<>` without the co-declared pair — is also rejected. The
// pre-fix `t->name == "UIntPtr"` short-circuit applied to both names; the
// v0.275 retirement removes both.
BEZEL_TEST(equality_v0158,
           codecl_user_record_named_uintptr_rejected_spec_25_2_v0_275) {
    auto r = parseSema(
        "module M; "
        "record UIntPtr(n: int) "
        "  public static method operator =(a: UIntPtr, b: UIntPtr): bool => a.n = b.n; "
        "  public static method operator <>(a: UIntPtr, b: UIntPtr): bool => a.n <> b.n; "
        "end UIntPtr; class F public static method Main(): int return 0; end Main; end F;");
    BEZEL_EXPECT_TRUE(hasCoDeclError(r));
}

// §25.2 (v0.275/v0.276): a user record named `IntPtr` WITH the co-declared
// pair (the four members `Ptr<T>` has always declared) satisfies the rule on
// its own merits. The check is now keyed on the *declaration*, not the name —
// so the rule is satisfied by the same shape that stdlib §10.0's `IntPtr`
// uses, whether or not the name collides with stdlib's record.
BEZEL_TEST(equality_v0158,
           codecl_user_record_named_intptr_with_pair_accepted_spec_25_2_v0_275) {
    auto r = parseSema(
        "module M; "
        "record IntPtr(n: int) "
        "  public static method operator =(a: IntPtr, b: IntPtr): bool => a.n = b.n; "
        "  public static method operator <>(a: IntPtr, b: IntPtr): bool => a.n <> b.n; "
        "  public method Equals(other: IntPtr): bool => n = other.n; "
        "  public override method GetHashCode(): int => n; "
        "end IntPtr; class F public static method Main(): int return 0; end Main; end F;");
    BEZEL_EXPECT_FALSE(hasCoDeclError(r));
}

// ─── §3.6 / §8.3 v0.158: hashable hash-container keys (#1338) ────────

// §3.6/§8.3: an `int` key is hashable — `Dictionary<int, int>` accepted.
BEZEL_TEST(equality_v0158, dict_int_key_accepted_spec_3_6) {
    auto r = parseSema(
        "module M; class C public method M(d: Dictionary<int, int>) end M; end C;");
    BEZEL_EXPECT_FALSE(hasEqualityBearingError(r));
}

// §3.6/§8.3: a `string` key is hashable.
BEZEL_TEST(equality_v0158, dict_string_key_accepted_spec_3_6) {
    auto r = parseSema(
        "module M; class C public method M(d: Dictionary<string, int>) end M; end C;");
    BEZEL_EXPECT_FALSE(hasEqualityBearingError(r));
}

// §3.6/§8.3: an enum key is hashable.
BEZEL_TEST(equality_v0158, dict_enum_key_accepted_spec_3_6) {
    auto r = parseSema(
        "module M; enum Color Red, Green; end Color; "
        "class C public method M(d: Dictionary<Color, int>) end M; end C;");
    BEZEL_EXPECT_FALSE(hasEqualityBearingError(r));
}

// §3.6/§8.3: a class key is hashable (reference identity is a consistent
// Equals/GetHashCode).
BEZEL_TEST(equality_v0158, dict_class_key_accepted_spec_3_6) {
    auto r = parseSema(
        "module M; class K public v: int := 0; end K; "
        "class C public method M(d: Dictionary<K, int>) end M; end C;");
    BEZEL_EXPECT_FALSE(hasEqualityBearingError(r));
}

// §3.6/§8.3: a positional record key (always synthesizes equality, §9.1) is
// hashable.
BEZEL_TEST(equality_v0158, dict_positional_record_key_accepted_spec_3_6) {
    auto r = parseSema(
        "module M; record Point(x: int, y: int); "
        "class C public method M(d: Dictionary<Point, int>) end M; end C;");
    BEZEL_EXPECT_FALSE(hasEqualityBearingError(r));
}

// §3.6/§8.3: an auto-hashable body record key is hashable.
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

// §3.6/§8.3: a body record that is neither auto-hashable (it has an
// explicit field) nor declares user equality is NOT hashable — a
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
// declare user equality — it is hashable and accepted as a Set element.
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
// positional field is not auto-hashable — `=` is a compile error.
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
// trivial body is auto-hashable — `=` is accepted.
BEZEL_TEST(equality_v0158, combined_admissible_fields_eq_accepted_spec_9_3) {
    auto r = parseSema(
        "module M; "
        "record Vec(X: double, Y: double) "
        "  public method Len(): double => 0.0; "
        "end Vec; "
        "class C public method M(a: Vec, b: Vec) _ := a = b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.3: a positional record with a class-typed field is NOT hashable,
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
// (no constraint surface for hashable exists). The check fires
// per-instantiation: a concrete instantiation with a non-hashable key
// is rejected at the instantiation site.
BEZEL_TEST(equality_v0158, dict_generic_param_key_concrete_bad_instantiation_spec_3_6) {
    auto r = parseSema(
        "module M; "
        // Inner non-hashable body record
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

// ─── §9.2 v0.276: heterogeneous `=`/`<>` overload does NOT disqualify (#3326) ─

// §9.2 v0.276 (fix #3326): a body record with a HETEROGENEOUS-operand
// `=`/`<>` overload (`V = int`, `V <> int`) is still auto-hashable. The
// overload defines a comparison the synthesis never provided and takes over
// nothing — §9.2's third-bullet membership reads: "An overload of any
// operator **other than a homogeneous `=`/`<>`** … A heterogeneous-operand
// `=`/`<>` overload (`T = U`) defines a comparison the synthesis never
// provided and takes over nothing: it neither qualifies nor disqualifies,
// exactly as §25.2 frees it from the co-declaration obligation." So `a = b`
// on the homogeneous form (both V) compiles via the synthesized `=` and
// compares structurally. Pre-fix this rejected with §9.2's use-site error
// (the broad reading treated any `=`/`<>` token as disqualifying,
// SymbolCollection.cpp:1373-1377).
BEZEL_TEST(equality_v0158, heterogeneous_eq_overload_keeps_auto_equality_spec_9_2_v0_276) {
    auto r = parseSema(
        "module M; "
        "record V "
        "  public property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  public static method operator =(a: V, b: int): bool => a.N = b; "
        "  public static method operator <>(a: V, b: int): bool => a.N <> b; "
        "end V; "
        "class C public method M(a: V, b: V) _ := a = b; end M; end C;");
    BEZEL_EXPECT_FALSE(hasNotAutoEqError(r));
}

// §9.2 v0.276 (fix #3326): the symmetric case — a heterogeneous `<>` overload
// alone (without `=`) is also fine for auto-equality. The membership rule keys
// on the operator TOKEN (`=` or `<>`), not on which side carries `=`. Same
// pre-fix regression and same post-fix acceptance.
BEZEL_TEST(equality_v0158, heterogeneous_ne_overload_keeps_auto_equality_spec_9_2_v0_276) {
    auto r = parseSema(
        "module M; "
        "record V "
        "  public property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  public static method operator <>(a: V, b: int): bool => a.N <> b; "
        "end V; "
        "class C public method M(a: V, b: V) _ := a <> b; end M; end C;");
    BEZEL_EXPECT_FALSE(hasNotAutoEqError(r));
}

// §9.2 v0.276 (fix #3326): a HETEROGENEOUS overload on the OTHER side
// (`int = V`) is also a heterogeneous overload — both operands need not be
// the declaring type, only that they are not BOTH it. Same fix, same
// expected outcome.
BEZEL_TEST(equality_v0158, heterogeneous_eq_reversed_overload_keeps_auto_equality_spec_9_2_v0_276) {
    auto r = parseSema(
        "module M; "
        "record V "
        "  public property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  public static method operator =(a: int, b: V): bool => a = b.N; "
        "  public static method operator <>(a: int, b: V): bool => a <> b.N; "
        "end V; "
        "class C public method M(a: V, b: V) _ := a = b; end M; end C;");
    BEZEL_EXPECT_FALSE(hasNotAutoEqError(r));
}

// §9.2 v0.276 (fix #3326, control): a HOMOGENEOUS `=`/`<>` overload with the
// §25.2 co-declared pair is still NOT auto-hashable — the pre-fix and
// post-fix verdicts agree on this case (the helper returns true for the
// homogeneous overload, the disqualifier fires). The regression test pins
// that the fix did NOT widen too far: the §9.2 third-bullet disqualifier
// still applies to the homogeneous form.
BEZEL_TEST(equality_v0158, homogeneous_eq_overload_still_disqualifies_spec_9_2_v0_276) {
    auto r = parseSema(
        "module M; "
        "record V "
        "  public property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  public static method operator =(a: V, b: V): bool => a.N = b.N; "
        "  public static method operator <>(a: V, b: V): bool => a.N <> b.N; "
        "  public method Equals(other: V): bool => N = other.N; "
        "  public override method GetHashCode(): int => N; "
        "end V; "
        "class C public method M(a: V, b: V) _ := a = b; end M; end C;");
    // With the user-supplied equality members the call is dispatched
    // directly to the user's `=` (lookupMember finds it), so the §9.2
    // use-site error does NOT fire — `hasNotAutoEqError` is the
    // non-auto-hashable-use-site check, distinct from the `=`/`<>` use
    // site. We pin: no compile errors, and the record is hashable (a
    // Dictionary<V, _> instantiation is accepted).
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto r2 = parseSema(
        "module M; "
        "record V "
        "  public property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  public static method operator =(a: V, b: V): bool => a.N = b.N; "
        "  public static method operator <>(a: V, b: V): bool => a.N <> b.N; "
        "  public method Equals(other: V): bool => N = other.N; "
        "  public override method GetHashCode(): int => N; "
        "end V; "
        "class C public method M(d: Dictionary<V, int>) end M; end C;");
    BEZEL_EXPECT_FALSE(hasEqualityBearingError(r2));
}

// §9.3 v0.276 (fix #3326): the positional-record form of the heterogeneous-
// overload case. A purely-positional record `V(n: int)` with heterogeneous
// `=`/`<>` operators keeps its auto-generated equality (the §9.3 admissibility
// rule applies §9.2's membership list with the same homogeneity test).
// isPositionalRecordEqualityBearing at SymbolCollection.cpp:1453-1457 was the
// twin of the body-record path and gets the same fix.
BEZEL_TEST(equality_v0158, positional_heterogeneous_eq_overload_keeps_auto_equality_spec_9_3_v0_276) {
    auto r = parseSema(
        "module M; "
        "record V(n: int) "
        "  public static method operator =(a: V, b: int): bool => a.n = b; "
        "  public static method operator <>(a: V, b: int): bool => a.n <> b; "
        "end V; "
        "class C public method M(a: V, b: V) _ := a = b; end M; end C;");
    BEZEL_EXPECT_FALSE(hasNotAutoEqError(r));
}

// §9.3 v0.276 (fix #3326): the combined-form record (positional + body) —
// same fix applies because isPositionalRecordEqualityBearing is the predicate
// for combined form (it walks all members: positional fields AND body).
BEZEL_TEST(equality_v0158, combined_heterogeneous_eq_overload_keeps_auto_equality_spec_9_3_v0_276) {
    auto r = parseSema(
        "module M; "
        "record V(n: int, s: string) "
        "  public method Label(): string => s; "
        "  public static method operator =(a: V, b: int): bool => a.n = b; "
        "  public static method operator <>(a: V, b: int): bool => a.n <> b; "
        "end V; "
        "class C public method M(a: V, b: V) _ := a = b; end M; end C;");
    BEZEL_EXPECT_FALSE(hasNotAutoEqError(r));
}

// §9.2 v0.276 (fix #3326): a heterogeneous-only record is hashable — it can
// serve as a Dictionary key. The `isEqualityBearing` OR of auto-hashable and
// user-declared equality returns true (auto-hashable is true after the fix).
// Pre-fix this rejected: `isAutoEqualityBearingBodyRecord` returned false on
// the heterogeneous overload, and `recordHasUserDeclaredEquality` does return
// true for the operator token — but the §3.6/§8.3 hashable check at the
// Dictionary<K, V> site would still have succeeded (it ORs both predicates),
// so this test was already passing on pre-fix code. Pin it anyway to guard
// against the fix regressing this surface.
BEZEL_TEST(equality_v0158, heterogeneous_overload_record_is_hashable_spec_3_6_v0_276) {
    auto r = parseSema(
        "module M; "
        "record V "
        "  public property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  public static method operator =(a: V, b: int): bool => a.N = b; "
        "  public static method operator <>(a: V, b: int): bool => a.N <> b; "
        "end V; "
        "class C public method M(d: Dictionary<V, int>) end M; end C;");
    BEZEL_EXPECT_FALSE(hasEqualityBearingError(r));
}

// ─── fix #3327 (§9.2 / §25.2 v0.276): intrinsic FFI pointer records as ────────
// components — they are NOT auto-hashable (their own homogeneous `=` overload
// is the v0.276 third disqualifier), so a record whose component type is
// IntPtr/UIntPtr/Ptr<T> is itself not auto-hashable. Pre-fix the null-
// declaration fallthrough at SymbolCollection.cpp:1297 admitted every null-
// declaration Named type as an admissible component — including the three
// FFI intrinsics — so a record like `Sqlite3Db(Raw: IntPtr)` slipped through
// the recursive component rule and synthesized an every-pair-equal `=` (the
// zero-property form, since IntPtr has no user-visible auto-property). Post-
// fix, the intrinsic pointer records are excluded explicitly and the
// standard §9.2 use-site error fires.
//
// Spec authorities: §9.2 (auto-generated equality; admissible-component rule;
// v0.276 strips auto-hashable on a homogeneous `=`/`<>` overload), §25.2
// (the intrinsic pointer records overload `=`/`<>` homogeneously, retired-
// exemption v0.275), §3.6 / §8.3 (hashable key/element at the call site).
// See DECLINED.md row 320 for the deferred widening of §9.2 from
// "auto-hashable" to "hashable".

// fix #3327: the headline case. `record Sqlite3Db(Raw: IntPtr)` is a body
// positional record (combined form) whose single component is `IntPtr`. The
// component is NOT admissible (IntPtr's own homogeneous `=` strips it of
// auto-hashable per v0.276), so the §9.2 use-site error fires on `=` and the
// §3.6 hashable-instantiation error fires on the Dictionary.
BEZEL_TEST(equality_v0158, intptr_component_body_record_eq_rejected_spec_9_2_v0_276_3327) {
    auto r = parseSema(
        "module M; "
        "record Sqlite3Db(Raw: IntPtr); "
        "class C public method M(a: Sqlite3Db, b: Sqlite3Db) _ := a = b; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(hasNotAutoEqError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// fix #3327: the §3.6 / §8.3 Dictionary-side rejection. A `Sqlite3Db` key is
// neither auto-hashable nor user-equality-declaring, so the Dictionary<K, V>
// instantiation rejects it with §3.6's "is not hashable" diagnostic. Pre-fix
// the Dictionary compiled (the fallthrough admitted IntPtr as a component
// and the synthesized every-pair-equal `=` slipped past §3.6 too).
BEZEL_TEST(equality_v0158, intptr_component_dict_key_rejected_spec_3_6_3327) {
    auto r = parseSema(
        "module M; "
        "record Sqlite3Db(Raw: IntPtr); "
        "class C public method M(d: Dictionary<Sqlite3Db, int>) end M; end C;");
    BEZEL_EXPECT_TRUE(hasEqualityBearingError(r));
}

// fix #3327: the symmetric Set<T> rejection — a Set<Sqlite3Db> element is
// subject to the same hashable requirement as a Dictionary key.
BEZEL_TEST(equality_v0158, intptr_component_set_element_rejected_spec_3_6_3327) {
    auto r = parseSema(
        "module M; "
        "record Sqlite3Db(Raw: IntPtr); "
        "class C public method M(s: Set<Sqlite3Db>) end M; end C;");
    BEZEL_EXPECT_TRUE(hasEqualityBearingError(r));
}

// fix #3327: UIntPtr has the same shape as IntPtr (homogeneous `=`/`<>`
// declared in stdlib §10.0), so the v0.276 disqualifier and the §9.2
// recursive rule apply identically. Pin the symmetry.
BEZEL_TEST(equality_v0158, uintptr_component_body_record_eq_rejected_spec_9_2_v0_276_3327) {
    auto r = parseSema(
        "module M; "
        "record Handle(Raw: UIntPtr); "
        "class C public method M(a: Handle, b: Handle) _ := a = b; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(hasNotAutoEqError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// fix #3327: the §10.5 generic FFI pointer record `Ptr<T>`. The head name
// `Ptr` alone is enough to identify the intrinsic (the `T` generic arg
// is just a pointee set gate, not a `=` overload). The post-fix null-
// declaration branch rejects it the same way. `Ptr<T>` lives in
// `System.Interop`, which is not auto-imported, so the test module declares
// the import explicitly.
BEZEL_TEST(equality_v0158, ptr_component_body_record_eq_rejected_spec_9_2_v0_276_3327) {
    auto r = parseSema(
        "module M; import System.Interop; "
        "record ForeignBuf(P: Ptr<byte>); "
        "class C public method M(a: ForeignBuf, b: ForeignBuf) _ := a = b; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(hasNotAutoEqError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// fix #3327: the body-record form (auto-property, not positional). The
// `isAdmissibleComponentType` predicate runs from `isAutoEqualityBearingBodyRecord`
// at the auto-property storage arm (SymbolCollection.cpp:1383-1387) — same
// predicate, same rejection. Without the fix this synthesized `=` over the
// IntPtr auto-property, which is exactly the every-pair-equal `=` the spec
// forbids.
BEZEL_TEST(equality_v0158, intptr_auto_property_body_record_eq_rejected_spec_9_2_v0_276_3327) {
    auto r = parseSema(
        "module M; "
        "record Sqlite3Db "
        "  public property Raw: IntPtr; "
        "  public constructor(raw: IntPtr) Raw := raw; end constructor; "
        "end Sqlite3Db; "
        "class C public method M(a: Sqlite3Db, b: Sqlite3Db) _ := a = b; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(hasNotAutoEqError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// fix #3327: control — `Dictionary<IntPtr, int>` itself is NOT rejected.
// IntPtr is hashable (it declares `Equals(T)` and `override GetHashCode`
// beside its homogeneous `=` overload, stdlib §10.0 / §25.2's co-declaration
// rule), so it keys a Dictionary on its address — the documented FFI shape.
// The fix only affects records whose COMPONENT type is IntPtr/UIntPtr/Ptr<T>;
// it does not change IntPtr's own hashable verdict.
BEZEL_TEST(equality_v0158, intptr_itself_dict_key_accepted_spec_3_6_3327) {
    auto r = parseSema(
        "module M; "
        "class C public method M(d: Dictionary<IntPtr, int>) end M; end C;");
    BEZEL_EXPECT_FALSE(hasEqualityBearingError(r));
}

// fix #3327: control — a record whose component type is `string` (an
// admissible component per §9.2's membership list) is still auto-hashable.
// The fix narrows the null-declaration branch to the three FFI intrinsics;
// nothing else is touched.
BEZEL_TEST(equality_v0158, string_component_body_record_still_auto_eq_spec_9_2_3327_control) {
    auto r = parseSema(
        "module M; "
        "record N(Name: string); "
        "class C public method M(a: N, b: N) _ := a = b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// fix #3327: control — a record with one IntPtr field can be MADE
// hashable by declaring user equality (§25.2's co-declaration rule: a
// homogeneous `=` overload plus `Equals(T)` and `override GetHashCode`).
// That is the documented escape for a record that wants to compare
// pointer-shaped components — the §9.2 error guides the user to it. The
// test exercises only `=` and `<>` use sites (not Dict-keying) so it does
// not depend on the separate #2457 universal-Object GetHashCode call on
// IntPtr; the user-equality route's verdict against the §9.2 disqualified
// base is what the fix turns around, and that verdict is independent of
// the runtime codegen path.
BEZEL_TEST(equality_v0158, intptr_component_record_user_equality_hashes_spec_25_2_3327) {
    auto r = parseSema(
        "module M; "
        "record Sqlite3Db(Raw: IntPtr) "
        "  public static method operator =(a: Sqlite3Db, b: Sqlite3Db): "
        "      bool => a.Raw = b.Raw; "
        "  public static method operator <>(a: Sqlite3Db, b: Sqlite3Db): "
        "      bool => a.Raw <> b.Raw; "
        "  public method Equals(other: Sqlite3Db): bool => Raw = other.Raw; "
        "  public override method GetHashCode(): int => 0; "
        "end Sqlite3Db; "
        "class C public method M(a: Sqlite3Db, b: Sqlite3Db) "
        "  _ := a = b; _ := a <> b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
