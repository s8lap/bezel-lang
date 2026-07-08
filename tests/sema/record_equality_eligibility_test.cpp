// Spec-derived tests (#348) — §9.2 / §8.3: value-equality operations on a body
// record that is NOT auto-equality-bearing and declares no equality of its own
// are a compile error at the use site.
//
// §9.2 "A body record is **not** auto-equality-bearing — and `=` on it without
// an explicit overload is a compile error — when it contains: an explicit field
// declaration (storage that is not an auto-property) …" and "The compile error
// applies to the `=` and `<>` operators **and to direct calls of
// `.Equals(Object?)` and `.GetHashCode()`** on the record. Calling
// `someRecord.Equals(other)` or `someRecord.GetHashCode()` on a
// non-auto-equality-bearing record without a user-defined override is a compile
// error …"
//
// §8.3 table: "Body-only record | Compile error if `=`/`<>` used; no default".
//
// §9.2 "Authors who want explicit equality on a record outside the
// auto-equality-bearing set declare `=` and `<>` overloads in the body, along
// with `Equals(Object?)`, `Equals(T)`, and `GetHashCode` overrides as needed."
// — such a record is excluded from the error (the operation dispatches to the
// user's member).
//
// Positional records (§9.1), enums (§10), and eligible body records (§9.2) get
// value equality and are unaffected.

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

// An ineligible body record: an explicit field declaration (storage that is not
// an auto-property) disqualifies auto-equality (§9.2).
constexpr const char* kIneligible =
    "record HasField "
    "  public property X: int; "
    "  private _cache: int := 0; "
    "  public constructor(x: int) X := x; end constructor; "
    "end HasField; ";

// An eligible body record: only auto-properties + a property-initializing
// constructor — auto-equality-bearing (§9.2).
constexpr const char* kEligible =
    "record Pt "
    "  public property X: int; "
    "  public property Y: int; "
    "  public constructor(x: int, y: int) X := x; Y := y; end constructor; "
    "end Pt; ";

} // namespace

// ─── Diagnosed: §9.2 error on the ineligible body record ───────────────────

// §9.2: `=` on a non-auto-equality-bearing body record is a compile error.
BEZEL_TEST(sema_record_equality, ineligible_eq_diagnosed_spec_9_2) {
    auto r = parseSema(
        std::string("module M; ") + kIneligible +
        "class C public method M() "
        "  var a := new HasField(1); var b := new HasField(1); "
        "  _ := a = b; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §9.2: `<>` on a non-auto-equality-bearing body record is a compile error.
BEZEL_TEST(sema_record_equality, ineligible_neq_diagnosed_spec_9_2) {
    auto r = parseSema(
        std::string("module M; ") + kIneligible +
        "class C public method M() "
        "  var a := new HasField(1); var b := new HasField(1); "
        "  _ := a <> b; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §9.2 / §8.3: `.Equals(...)` on a non-auto-equality-bearing body record is a
// compile error at the call site.
BEZEL_TEST(sema_record_equality, ineligible_equals_call_diagnosed_spec_8_3) {
    auto r = parseSema(
        std::string("module M; ") + kIneligible +
        "class C public method M() "
        "  var a := new HasField(1); var b := new HasField(1); "
        "  _ := a.Equals(b); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §9.2 / §8.3: `.GetHashCode()` on a non-auto-equality-bearing body record is a
// compile error at the call site.
BEZEL_TEST(sema_record_equality, ineligible_gethashcode_call_diagnosed_spec_8_3) {
    auto r = parseSema(
        std::string("module M; ") + kIneligible +
        "class C public method M() "
        "  var a := new HasField(1); "
        "  _ := a.GetHashCode(); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §8.5 / §9.2: `.ToString()` on an ineligible body record is NOT an error — the
// type-name default ToString applies. Only equality is gated.
BEZEL_TEST(sema_record_equality, ineligible_tostring_call_accepted_spec_8_5) {
    auto r = parseSema(
        std::string("module M; ") + kIneligible +
        "class C public method M() "
        "  var a := new HasField(1); "
        "  _ := a.ToString(); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ─── Controls: value equality remains accepted ─────────────────────────────

// §9.2: an eligible body record gets auto-equality — `=` is accepted.
BEZEL_TEST(sema_record_equality, eligible_eq_accepted_spec_9_2) {
    auto r = parseSema(
        std::string("module M; ") + kEligible +
        "class C public method M() "
        "  var a := new Pt(1, 2); var b := new Pt(1, 2); "
        "  _ := a = b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.2 / §8.3: an eligible body record's `.Equals` / `.GetHashCode` are
// accepted.
BEZEL_TEST(sema_record_equality, eligible_equals_gethashcode_accepted_spec_8_3) {
    auto r = parseSema(
        std::string("module M; ") + kEligible +
        "class C public method M() "
        "  var a := new Pt(1, 2); var b := new Pt(1, 2); "
        "  _ := a.Equals(b); _ := a.GetHashCode(); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.1: a positional record gets field-by-field value equality — `=`,
// `.Equals`, and `.GetHashCode` are accepted.
BEZEL_TEST(sema_record_equality, positional_eq_accepted_spec_9_1) {
    auto r = parseSema(
        "module M; record Point(x: int, y: int); "
        "class C public method M() "
        "  var a := new Point(1, 2); var b := new Point(1, 2); "
        "  _ := a = b; _ := a.Equals(b); _ := a.GetHashCode(); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.2: an ineligible body record (explicit field) that declares its own
// `Equals` is excluded from the error — the call dispatches to the user member.
BEZEL_TEST(sema_record_equality, ineligible_user_equals_accepted_spec_9_2) {
    auto r = parseSema(
        "module M; "
        "record Tagged "
        "  public property X: int; "
        "  private _cache: int := 0; "
        "  public constructor(x: int) X := x; end constructor; "
        "  public method Equals(other: Tagged): bool => true; "
        "  public method GetHashCode(): int => 0; "
        "end Tagged; "
        "class C public method M() "
        "  var a := new Tagged(1); var b := new Tagged(2); "
        "  _ := a.Equals(b); _ := a.GetHashCode(); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.2 / §8.4: an ineligible body record that declares its own `=` / `<>`
// operator overloads is excluded from the error — `=` dispatches to the
// user-declared operator.  Per §25.2 v0.158 the homogeneous `=`/`<>` overload
// requires a co-declared `Equals(T)` + `override GetHashCode`.
BEZEL_TEST(sema_record_equality, ineligible_user_operator_eq_accepted_spec_8_4) {
    auto r = parseSema(
        "module M; "
        "record Tagged "
        "  public property X: int; "
        "  private _cache: int := 0; "
        "  public constructor(x: int) X := x; end constructor; "
        "  public static method operator =(a: Tagged, b: Tagged): bool => true; "
        "  public static method operator <>(a: Tagged, b: Tagged): bool => false; "
        "  public method Equals(other: Tagged): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end Tagged; "
        "class C public method M() "
        "  var a := new Tagged(1); var b := new Tagged(2); "
        "  _ := a = b; _ := a <> b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ─── v0.182 (#1572): #770's "non-trivial constructor body" disqualifier is
// ─── RETIRED — a constructor never disqualifies auto-equality, regardless of
// ─── what its body does ──────────────────────────────────────────────────

// §9.2 v0.182 (#1572): "Removed in v0.182: the former … 'non-trivial
// constructor body' disqualifier[]. … no constructor body … can change what
// structural `=`, `GetHashCode`, or `ToString` mean." A constructor with
// control flow (`if … then … end`) — the exact #770 example that used to be
// rejected — no longer disqualifies: `=` compiles.
// (Regression guard, superseding the retired #770
// nontrivial_ctor_eq_diagnosed_770 test, which asserted the now-repealed
// behavior.)
BEZEL_TEST(sema_record_equality, nontrivial_ctor_eq_accepted_v0182_1572) {
    auto r = parseSema(
        "module M; "
        "record Clamped "
        "  public property V: int; "
        "  public constructor(v: int) "
        "    V := v; "
        "    if V > 100 then V := 100; end; "
        "  end constructor; "
        "end Clamped; "
        "class C public method M() "
        "  var a := new Clamped(5); var b := new Clamped(5); "
        "  _ := a = b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.2 v0.182 (#1572): `.Equals(...)` on the same record is likewise accepted
// now that a non-trivial constructor body no longer disqualifies.
// (Regression guard, superseding the retired #770
// nontrivial_ctor_equals_call_diagnosed_770 test.)
BEZEL_TEST(sema_record_equality, nontrivial_ctor_equals_call_accepted_v0182_1572) {
    auto r = parseSema(
        "module M; "
        "record Clamped "
        "  public property V: int; "
        "  public constructor(v: int) "
        "    V := v; "
        "    if V > 100 then V := 100; end; "
        "  end constructor; "
        "end Clamped; "
        "class C public method M() "
        "  var a := new Clamped(5); var b := new Clamped(5); "
        "  _ := a.Equals(b); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.2 v0.182 (#1572): a VALIDATING constructor — the spec's own motivating
// example ("Width > 0 and Height > 0"), which THROWS on invalid input rather
// than merely clamping — is likewise fully compatible with auto-equality.
BEZEL_TEST(sema_record_equality, validating_ctor_eq_accepted_v0182_1572) {
    auto r = parseSema(
        "module M; "
        "record Rectangle "
        "  public property Width: decimal; "
        "  public property Height: decimal; "
        "  public constructor(width: decimal, height: decimal) "
        "    if width <= 0 or height <= 0 then "
        "      throw new Exception(\"bad dims\"); "
        "    end; "
        "    Width := width; "
        "    Height := height; "
        "  end constructor; "
        "end Rectangle; "
        "class C public method M() "
        "  var a := new Rectangle(3, 4); var b := new Rectangle(3, 4); "
        "  _ := a = b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ─── v0.182 (#1572): static members are ignored — neither qualify nor
// ─── disqualify auto-equality ───────────────────────────────────────────

// §9.2 v0.182: "Static members are ignored … `static readonly` fields …
// contribute no instance storage and neither qualify nor disqualify the
// record — a record may carry `Origin`-style constants … without losing `=`."
// A `static readonly` FIELD (an explicit FieldDecl, not an auto-property) must
// NOT disqualify — before the fix, the FieldSymbol arm of
// isAutoEqualityBearingBodyRecord rejected ANY explicit field unconditionally,
// including static ones.
BEZEL_TEST(sema_record_equality, static_readonly_field_ignored_eq_accepted_v0182_1572) {
    auto r = parseSema(
        "module M; "
        "record Point "
        "  public property X: int; "
        "  public property Y: int; "
        "  public static readonly Label: string := \"origin-family\"; "
        "  public constructor(x: int, y: int) X := x; Y := y; end constructor; "
        "end Point; "
        "class C public method M() "
        "  var a := new Point(1, 2); var b := new Point(1, 2); "
        "  _ := a = b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.2 v0.182 (via §9.3's combined-form application of the same rule): a
// static field is ignored on a COMBINED-form (positional + body) record too.
BEZEL_TEST(sema_record_equality,
           static_readonly_field_ignored_combined_form_eq_accepted_v0182_1572) {
    auto r = parseSema(
        "module M; "
        "record Vec2(X: int, Y: int) "
        "  public static readonly Label: string := \"vec2\"; "
        "end Vec2; "
        "class C public method M() "
        "  var a := new Vec2(1, 2); var b := new Vec2(1, 2); "
        "  _ := a = b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Regression control: a NON-static explicit field still disqualifies (§9.2
// unchanged) — only `static` fields get the v0.182 exemption.
BEZEL_TEST(sema_record_equality,
           non_static_field_still_disqualifies_control_v0182_1572) {
    auto r = parseSema(
        "module M; "
        "record HasNonStaticField "
        "  public property X: int; "
        "  public Cache: int := 0; "
        "  public constructor(x: int) X := x; end constructor; "
        "end HasNonStaticField; "
        "class C public method M() "
        "  var a := new HasNonStaticField(1); var b := new HasNonStaticField(1); "
        "  _ := a = b; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §9.2 (#770 regression guard): a TRIVIAL constructor — only property-
// initialization assignments (`X := x;` / `this.Y := y;`) — does NOT disqualify
// auto-equality; `=` remains available. (Must not over-reject the common form.)
BEZEL_TEST(sema_record_equality, trivial_ctor_eq_accepted_770) {
    auto r = parseSema(
        "module M; "
        "record Pt2 "
        "  public property X: int; "
        "  public property Y: int; "
        "  public constructor(x: int, y: int) X := x; this.Y := y; end constructor; "
        "end Pt2; "
        "class C public method M() "
        "  var a := new Pt2(1, 2); var b := new Pt2(1, 2); "
        "  _ := a = b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ─── #1249: §9.2 v0.156 — override Equals/GetHashCode disqualifies ──────────

// §9.2 v0.156 / #1249: a body record that declares `override Equals`
// (with its required companion `override GetHashCode`) WITHOUT `operator =`/`<>`
// is NOT auto-equality-bearing — `=` is a compile error.
BEZEL_TEST(sema_record_equality, override_equals_gethashcode_eq_diagnosed_1249) {
    auto r = parseSema(
        "module M; "
        "record R "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public override method Equals(other: Object?): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end R; "
        "class C public method M() "
        "  var a := new R(1); var b := new R(9); "
        "  _ := a = b; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §9.2 v0.156 / #1249: a body record with `override Equals` alone (without
// `operator =`/`<>`) is not auto-equality-bearing — `=` is a compile error.
// (The §8.3 eq+hash pairing error is also expected, but the §9.2 `=` reject
// is the load-bearing assertion here.)
BEZEL_TEST(sema_record_equality, override_equals_only_eq_diagnosed_1249) {
    auto r = parseSema(
        "module M; "
        "record R "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public override method Equals(other: Object?): bool => true; "
        "end R; "
        "class C public method M() "
        "  var a := new R(1); var b := new R(9); "
        "  _ := a = b; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §9.2 v0.156 / #1249: a body record with `override GetHashCode` alone
// (without `operator =`/`<>`) is not auto-equality-bearing — `=` is a
// compile error.
BEZEL_TEST(sema_record_equality, override_gethashcode_only_eq_diagnosed_1249) {
    auto r = parseSema(
        "module M; "
        "record R "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public override method GetHashCode(): int => 0; "
        "end R; "
        "class C public method M() "
        "  var a := new R(1); var b := new R(9); "
        "  _ := a = b; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §9.2 v0.156 / #1249: a body record with BOTH `override Equals`/`GetHashCode`
// AND explicit `operator =`/`<>` is valid — the author opted in (§9.2 "Authors
// who want explicit equality … declare `=` and `<>`").  `=` compiles.
BEZEL_TEST(sema_record_equality, override_equals_plus_operator_eq_accepted_1249) {
    auto r = parseSema(
        "module M; "
        "record R "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public override method Equals(other: Object?): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "  public static method operator =(a: R, b: R): bool => a.V = b.V; "
        "  public static method operator <>(a: R, b: R): bool => a.V <> b.V; "
        "end R; "
        "class C public method M() "
        "  var a := new R(1); var b := new R(9); "
        "  _ := a = b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.2 v0.156 / #1249 regression: a plain auto-equality body record (only
// auto-properties and a trivial constructor) still gets auto-equality — `=`
// must NOT be rejected.
BEZEL_TEST(sema_record_equality, plain_auto_eq_unaffected_1249) {
    auto r = parseSema(
        "module M; "
        "record Pt "
        "  public property X: int; "
        "  public property Y: int; "
        "  public constructor(x: int, y: int) X := x; Y := y; end constructor; "
        "end Pt; "
        "class C public method M() "
        "  var a := new Pt(1, 2); var b := new Pt(1, 2); "
        "  _ := a = b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
