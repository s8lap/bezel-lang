// Spec-derived sema tests for the v0.276 choice equality ruling (fix #3333):
// §35.7 widens the homogeneous `=`/`<>` overload disqualifier from records to
// value-like choices; §25.2 widens its co-declaration rule from
// record/enum to record/enum/choice; §3.6's hashable row grows the "nor
// declares user equality" arm to choices, and choiceHasUserDeclaredEquality
// wires the rescue.
//
// Authorities (v0.276):
//   - §3.6 hashable row — adds the choice "nor declares user equality" arm.
//   - §25.2 co-declaration rule — adds choices; the choice spelling uses
//     plain `GetHashCode` (§35.2 carries §10.5's plain-member replacement
//     rule over). Heterogeneous-operand overloads (`T = U`) carry no
//     obligation; this matches the §9.2/§3326 record precedent.
//   - §35.7 disqualifier — a homogeneous `=`/`<>` overload disqualifies a
//     value-like choice exactly as it disqualifies a record; §25.2's
//     co-declaration rule then supplies the equality surface.
//
// Per CLAUDE.md and lane-quality-selfpass, this test pins every spec
// assertion: the four scenario axes below are the case grid the ruling
// names; the test text quotes each spec clause it verifies.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

// fix #3985: delegates to the shared gated helper (sema_support.hpp)
// instead of duplicating the parse-error-gate logic locally.
bezel::test::SemaResult semaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
}

// §25.2 co-declaration error: "a [record|enum|choice] that overloads `=`
// must also declare `Equals(T)` and `GetHashCode`".
bool hasCoDeclError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("must also declare `Equals(T)`") !=
                std::string::npos)
            return true;
    }
    return false;
}

// §35.7 use-site error on `=`/`<>` for a choice that is not auto-hashable
// AND not rescued by the §25.2 co-declared pair.
bool hasNotAutoEqError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("is not available on choice") !=
                std::string::npos)
            return true;
    }
    return false;
}

// §35.7 use-site error on direct `.Equals(...)`/`.GetHashCode()`.
bool hasNotAvailableMemberError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("is not available on choice") !=
                std::string::npos)
            return true;
    }
    return false;
}

// §3.6/§8.3 hashable instantiation error on a `Dictionary<K,V>` key.
bool hasHashableError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("is not hashable") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// ─── §25.2 (v0.276, #3333): co-declaration rule fires for CHOICES ─────────

// §25.2: a value-like CHOICE overloading `=`/`<>` (homogeneous operands)
// WITHOUT the co-declared `Equals(T)` + `GetHashCode` is a compile error.
// The diagnostic names the choice's own spelling — `GetHashCode` (no
// `override`), per §35.2's carry-over of §10.5's plain-member rule.
BEZEL_TEST(auto_hashable_choice_equality_3333,
           codecl_choice_missing_pair_rejected_spec_25_2_v0_276) {
    auto r = semaAll(
        "module M; "
        "choice Opt Some(value: int), None; "
        "  public static method operator =(a: Opt, b: Opt): bool => true; "
        "  public static method operator <>(a: Opt, b: Opt): bool => false; "
        "end Opt; "
        "class F public static method Main(): int return 0; "
        "end Main; end F;");
    BEZEL_EXPECT_TRUE(hasCoDeclError(r));
}

// §25.2: the choice's diagnostic must spell `GetHashCode` (no `override`),
// since §35.2 carries §10.5's plain-member replacement rule over to
// choices. The v0.183 fix at TypeResolution_Checks.cpp's enum arm set
// this precedent; the v0.276 widening (fix #3333) extends the same
// spelling to choices. At v0.276 this was required to avoid a second,
// unrelated compile error (the same defect class as #1381/#1582 for enums):
// `override` was rejected outright on any choice method. v0.288/#3837 later
// ADMITS `override` on a choice's `Equals`/`GetHashCode`/`ToString` too, but
// the plain spelling recommended below remains fully sufficient, so this
// message — and this test — are unaffected.
BEZEL_TEST(auto_hashable_choice_equality_3333,
           codecl_choice_message_says_plain_gethashcode_spec_25_2_v0_276) {
    auto r = semaAll(
        "module M; "
        "choice Opt Some(value: int), None; "
        "  public static method operator =(a: Opt, b: Opt): bool => true; "
        "  public static method operator <>(a: Opt, b: Opt): bool => false; "
        "end Opt; "
        "class F public static method Main(): int return 0; "
        "end Main; end F;");
    bool sawPlainSpelling = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("must also declare `Equals(T)`") ==
            std::string::npos)
            continue;
        sawPlainSpelling = d.message.find("`GetHashCode`") !=
                           std::string::npos;
        BEZEL_EXPECT_TRUE(d.message.find("override GetHashCode") ==
                          std::string::npos);
    }
    BEZEL_EXPECT_TRUE(sawPlainSpelling);
}

// §25.2: the same choice WITH the co-declared pair is accepted (no
// co-declaration error). The §25.2 obligation is discharged; per §3.6
// the choice is hashable on the strength of the pair.
BEZEL_TEST(auto_hashable_choice_equality_3333,
           codecl_choice_satisfied_accepted_spec_25_2_v0_276) {
    auto r = semaAll(
        "module M; "
        "choice Opt Some(value: int), None; "
        "  public static method operator =(a: Opt, b: Opt): bool => true; "
        "  public static method operator <>(a: Opt, b: Opt): bool => false; "
        "  public method Equals(other: Opt): bool => true; "
        "  public method GetHashCode(): int => 0; "
        "end Opt; "
        "class F public static method Main(): int return 0; "
        "end Main; end F;");
    BEZEL_EXPECT_FALSE(hasCoDeclError(r));
}

// §25.2: a HETEROGENEOUS-operand overload on a choice (`Opt = int`)
// carries no co-declaration obligation — no error even without the pair.
// Mirrors the record precedent (test equality_v0158.codecl_heterogeneous_
// exempt_spec_25_2, fix #3326).
BEZEL_TEST(auto_hashable_choice_equality_3333,
           codecl_choice_heterogeneous_exempt_spec_25_2_v0_276) {
    auto r = semaAll(
        "module M; "
        "choice Opt Some(value: int), None; "
        "  public static method operator =(a: Opt, b: int): bool => true; "
        "  public static method operator <>(a: Opt, b: int): bool => false; "
        "end Opt; "
        "class F public static method Main(): int return 0; "
        "end Main; end F;");
    BEZEL_EXPECT_FALSE(hasCoDeclError(r));
}

// ─── §35.7 (v0.276, #3333): use-site errors + the rescue clause ──────────

// §35.7: a value-like choice with homogeneous `=`/`<>` overload and NO
// co-declared pair is not auto-hashable — the §25.2 co-declaration error
// (test above) is the rejection. The §35.7 use-site error on `=`/`<>`
// itself is NOT raised: findBinaryOperator dispatches to the user's
// overload before the §35.7 use-site path runs. The §35.7 path is
// reachable only via the direct `.Equals(...)`/`.GetHashCode()` call
// path (next test), or on a non-value-like choice without an overload.
// This test pins that the `=` use site dispatches the user operator
// (post-#3333 the §25.2 error still fires; the `=` use itself compiles
// through the user's operator — the surface the spec rules).
BEZEL_TEST(auto_hashable_choice_equality_3333,
           choice_eq_overload_without_pair_dispatches_spec_35_7_v0_276) {
    auto r = semaAll(
        "module M; "
        "choice Opt Some(value: int), None; "
        "  public static method operator =(a: Opt, b: Opt): bool => true; "
        "  public static method operator <>(a: Opt, b: Opt): bool => false; "
        "end Opt; "
        "class C public static method Main(): int "
        "  var a: Opt := Opt.None; "
        "  var b: Opt := Opt.None; "
        "  if a = b then return 1; else return 0; end; "
        "end Main; end C;");
    // §35.7 binary `=`/`<>` use-site error does NOT fire (operator
    // dispatched). §25.2 co-declaration error DOES fire. Pin the latter.
    BEZEL_EXPECT_FALSE(hasNotAutoEqError(r));
    BEZEL_EXPECT_TRUE(hasCoDeclError(r));
}

// §35.7 (v0.276, #3333): the RESCUE clause — a value-like choice with
// the homogeneous `=`/`<>` overload AND the co-declared
// `Equals(T)`/`GetHashCode` pair is hashable (§3.6) and `=` is legal on
// it. Pre-#3333 the use-site check rejected the same shape (no OR with
// choiceHasUserDeclaredEquality), so the rescue clause was structurally
// unreachable. This test pins the post-#3333 acceptance.
BEZEL_TEST(auto_hashable_choice_equality_3333,
           choice_eq_overload_with_pair_rescues_eq_spec_35_7_v0_276) {
    auto r = semaAll(
        "module M; "
        "choice Opt Some(value: int), None; "
        "  public static method operator =(a: Opt, b: Opt): bool => true; "
        "  public static method operator <>(a: Opt, b: Opt): bool => false; "
        "  public method Equals(other: Opt): bool => true; "
        "  public method GetHashCode(): int => 0; "
        "end Opt; "
        "class C public static method Main(): int "
        "  var a: Opt := Opt.None; "
        "  var b: Opt := Opt.None; "
        "  if a = b then return 1; else return 0; end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNotAutoEqError(r));
}

// §35.7 (v0.276, #3333): same rescue, but the `=` use site is `<>`
// instead — symmetric, mirrors the record precedent for `<>`.
BEZEL_TEST(auto_hashable_choice_equality_3333,
           choice_ne_overload_with_pair_rescues_ne_spec_35_7_v0_276) {
    auto r = semaAll(
        "module M; "
        "choice Opt Some(value: int), None; "
        "  public static method operator =(a: Opt, b: Opt): bool => true; "
        "  public static method operator <>(a: Opt, b: Opt): bool => false; "
        "  public method Equals(other: Opt): bool => true; "
        "  public method GetHashCode(): int => 0; "
        "end Opt; "
        "class C public static method Main(): int "
        "  var a: Opt := Opt.None; "
        "  var b: Opt := Opt.None; "
        "  if a <> b then return 1; else return 0; end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNotAutoEqError(r));
}

// §35.7 (v0.276, #3333): the rescue clause for DIRECT
// `.Equals(...)`/`.GetHashCode()` calls — same predicate, same gate. The
// checkEqualityMemberUseSite site (ExprTypecheck_Calls.cpp) is OR'd with
// choiceHasUserDeclaredEquality, mirroring the record gate above it.
BEZEL_TEST(auto_hashable_choice_equality_3333,
           choice_eq_overload_with_pair_rescues_direct_equals_spec_35_7_v0_276) {
    auto r = semaAll(
        "module M; "
        "choice Opt Some(value: int), None; "
        "  public static method operator =(a: Opt, b: Opt): bool => true; "
        "  public static method operator <>(a: Opt, b: Opt): bool => false; "
        "  public method Equals(other: Opt): bool => true; "
        "  public method GetHashCode(): int => 0; "
        "end Opt; "
        "class C public static method Main(): int "
        "  var a: Opt := Opt.None; "
        "  return (if a.Equals(a) then 1 else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNotAvailableMemberError(r));
}

// ─── §3.6 (v0.276, #3333): hashable row accepts the rescue ────────────────

// §3.6: a value-like choice that has homogeneous `=`/`<>` AND the
// co-declared pair is hashable — keys a `Dictionary<Opt, _>`. Pre-#3333
// the Dictionary key check consulted only isAutoEqualityBearingChoice
// (no OR), so a non-auto-hashable choice was rejected here even when the
// §25.2 rescue was in place — the "unrescuable, no diagnostic remedy"
// defect the v0.276 composition gate flagged.
BEZEL_TEST(auto_hashable_choice_equality_3333,
           choice_with_overload_and_pair_keys_dictionary_spec_3_6_v0_276) {
    auto r = semaAll(
        "module M; "
        "choice Opt Some(value: int), None; "
        "  public static method operator =(a: Opt, b: Opt): bool => true; "
        "  public static method operator <>(a: Opt, b: Opt): bool => false; "
        "  public method Equals(other: Opt): bool => true; "
        "  public method GetHashCode(): int => 0; "
        "end Opt; "
        "class C public method M(d: Dictionary<Opt, int>) end M; end C;");
    BEZEL_EXPECT_FALSE(hasHashableError(r));
}

// §3.6 (v0.276, #3333): the same choice WITHOUT the pair is NOT hashable
// — keys a `Dictionary<Opt, _>` reject. Pre-#3333 the choice was
// declared auto-hashable and the key check ACCEPTED — the over-ACCEPT
// half of the spec-ahead-of-impl delta.
BEZEL_TEST(auto_hashable_choice_equality_3333,
           choice_with_overload_no_pair_not_hashable_spec_3_6_v0_276) {
    auto r = semaAll(
        "module M; "
        "choice Opt Some(value: int), None; "
        "  public static method operator =(a: Opt, b: Opt): bool => true; "
        "  public static method operator <>(a: Opt, b: Opt): bool => false; "
        "end Opt; "
        "class C public method M(d: Dictionary<Opt, int>) end M; end C;");
    BEZEL_EXPECT_TRUE(hasHashableError(r));
}

// ─── CONTROL: §35.7 unchanged for choices with NO `=`/`<>` overload ──────

// §35.7 (control): a value-like choice with NO `=`/`<>` overload and NO
// Equals/GetHashCode is auto-hashable, synthesized `=` is allowed, the
// Dictionary key is accepted. This was the working pre-#3333 behavior
// and must be preserved by the fix (pin so the disqualifier doesn't
// over-fire on benign bodies).
BEZEL_TEST(auto_hashable_choice_equality_3333,
           control_value_like_choice_no_overload_unchanged_spec_35_7) {
    auto r = semaAll(
        "module M; "
        "choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var a: Opt := Opt.None; "
        "  var b: Opt := Opt.None; "
        "  if a = b then return 1; else return 0; end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNotAutoEqError(r));
}

// §3.6 (control): the same choice keys a Dictionary — preserved
// pre-#3333 behavior.
BEZEL_TEST(auto_hashable_choice_equality_3333,
           control_value_like_choice_no_overload_keys_dictionary_spec_3_6) {
    auto r = semaAll(
        "module M; "
        "choice Opt Some(value: int), None; end Opt; "
        "class C public method M(d: Dictionary<Opt, int>) end M; end C;");
    BEZEL_EXPECT_FALSE(hasHashableError(r));
}

// ─── §3.6 boundary: the rescue is value-like only ─────────────────────────

// §3.6 hashable row: an OWNING choice with `=`/`<>` overload AND the
// co-declared pair is NOT hashable — the rescue applies ONLY to value-
// like choices ("a value-like choice outside auto-hashable only because
// it carries a homogeneous `=`/`<>` overload is hashable on the strength
// of … §25.2's co-declaration rule"; the same row's earlier clause:
// "owning and borrow-bearing choices are not hashable"). Pin that
// choiceHasUserDeclaredEquality gates on ValueLike — the owning branch
// is structural, not a §35.7 disqualifier to rescue.
BEZEL_TEST(auto_hashable_choice_equality_3333,
           owning_choice_with_pair_still_not_hashable_spec_3_6_v0_276) {
    auto r = semaAll(
        "module M; class R end R; "
        "choice Fetched Loaded(owned response: R), None; "
        "  public static method operator =(a: Fetched, b: Fetched): bool "
        "    => true; "
        "  public static method operator <>(a: Fetched, b: Fetched): bool "
        "    => false; "
        "  public method Equals(other: Fetched): bool => true; "
        "  public method GetHashCode(): int => 0; "
        "end Fetched; "
        "class C public method M(d: Dictionary<Fetched, int>) end M; end C;");
    BEZEL_EXPECT_TRUE(hasHashableError(r));
}