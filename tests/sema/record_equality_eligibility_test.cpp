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
// user-declared operator.
BEZEL_TEST(sema_record_equality, ineligible_user_operator_eq_accepted_spec_8_4) {
    auto r = parseSema(
        "module M; "
        "record Tagged "
        "  public property X: int; "
        "  private _cache: int := 0; "
        "  public constructor(x: int) X := x; end constructor; "
        "  public static method operator =(a: Tagged, b: Tagged): bool => true; "
        "  public static method operator <>(a: Tagged, b: Tagged): bool => false; "
        "end Tagged; "
        "class C public method M() "
        "  var a := new Tagged(1); var b := new Tagged(2); "
        "  _ := a = b; _ := a <> b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ─── #770: a NON-TRIVIAL constructor body disqualifies auto-equality ────────

// §9.2: "A body record is not auto-equality-bearing … when it contains: A
// non-trivial constructor body that performs work other than property
// initialization." A constructor with control flow (`if … then … end`) is
// non-trivial — `=` on the record is a compile error.
BEZEL_TEST(sema_record_equality, nontrivial_ctor_eq_diagnosed_770) {
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
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §9.2 / §8.3: `.Equals(...)` on the same non-trivial-ctor record is likewise a
// compile error.
BEZEL_TEST(sema_record_equality, nontrivial_ctor_equals_call_diagnosed_770) {
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
