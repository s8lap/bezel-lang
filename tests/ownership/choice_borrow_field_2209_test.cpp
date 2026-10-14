// Spec-derived tests for issue #2209 — §35.4 / §9.7 (v0.226): a borrow-bearing
// CHOICE stored directly in a class instance field is rejected, with the same
// null-source exception the §9.7 record rule already admits for records
// (`Common::isNullSourceRecordExpr`, #1540).
//
// §35.4: a choice is BORROW-BEARING iff any case has a plain (unmarked)
// borrow-carrying payload field (an InlineArray of reference elements, a
// class/interface reference field, or transitively a borrow-bearing record).
// §9.7 reads "A borrow-bearing record value may not be stored in an instance
// field of a class"; §35.4 makes the §9.7 rule apply to borrow-bearing choices
// by reading "record" as "record or choice". Before this fix the predicate
// site (`MethodOwnership::checkBorrowStoreEscape` at the `Dest::Field` leg,
// `checkFieldInitializer`, and `checkCollectionBorrowStoreEscape` at the
// `RecvKind::Escapes` leg) consulted only `isBorrowBearingRecord`, which
// never classifies a choice, so a `this.field := Choice.Borrowed(local)` (or
// any other construction of a choice whose active case carries a borrow-
// typed payload) silently escaped the check: the field's runtime value
// carried the borrow past the local's auto-drop, a §9.7 use-after-dispose.
// The fix adds a choice leg to all three rejection sites, parallel to the
// existing record leg, threaded through the same null-source exception path
// (`Common::isNullSourceChoiceExpr` — the parallel recognizer the lit tests
// cover).
//
// The REJECT side — a choice constructor stored into a class instance field,
// the field-initializer form, and an `Add` into a field-typed escaping
// borrowing collection — is pinned by the lit tests
// (tests/lit/ownership/choice_borrow_field_2209__*_rejected.bzl). This gtest
// pins the ACCEPT side — null-source choices stored into a field, value-like
// choices stored into a field, a method-local borrow-bearing choice that is
// NEVER stored into a field (the over-reject failure mode), and the
// continued rejection of the #2159 record-with-choice-field face — all
// compile cleanly with no spurious diagnostics.
//
// per §35.4 (v0.226), §9.7 (v0.226)

#include <string>

#include "support/ownership_support.hpp"  // parseAndCheckOwnership — sema + ownership
#include "support/aot_support.hpp"        // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

// True when an ERROR diagnostic whose message contains `sub` was emitted.
// Mirrors tests/ownership/record_field_collection_store_1540_test.cpp's
// `hasError` recognizer (sub-pin to the rule under test, never satisfied by
// an unrelated diagnostic).
static bool hasError(const OwnershipResult& r, const std::string& sub) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(sub) != std::string::npos) return true;
    }
    return false;
}

// `Res` is a disposable resource class; `Opt` is the canonical §35.4.2
// borrow-bearing choice (`Some(r: Res?)` carries an unmarked class payload).
// `VOpt` is a value-like choice (every payload is value-typed or runtime-
// managed, §35.4.3) — the "no over-rejection" control for the new predicate.
namespace {
const char* kCommon =
    "module M; "
    "class Res public constructor() end constructor; "
    "  destructor end destructor; end Res; "
    "choice Opt Some(r: Res?), None; end Opt; "
    "choice VOpt Some(n: int), Named(s: string), None; end VOpt; ";
std::string mod(const std::string& decls) {
    return std::string(kCommon) + decls;
}
} // namespace

// ===========================================================================
// POSITIVE — null-source choices stored into a class instance field are
// admitted (the §9.7 mirror of #1540's `new RefRec(null)` exemption for
// records). Mirrors `isNullSourceChoiceExpr`'s recognition shapes:
//   (a) A payload-free case reference (`Opt.None`) — no borrow to source,
//       trivially null-source. Mirrors `CancellationToken.None` (the stdlib
//       "sentinel" exception §9.7 documents for records).
//   (b) A case construction where the borrow-carrying payload is a literal
//       `null` (`Opt.Some(null)` — `r: Res?` carries the borrow; `null` is
//       the admitted source).
//
// Both shapes route through `MethodOwnership::checkBorrowStoreEscape` (the
// assignment-site checkFix touches via the `Dest::Field` leg). The
// `Opt.None` shape also exercises `isNullSourceChoiceExpr`'s (a) leg
// (MemberAccessExpr resolving to a payload-free ChoiceCaseSymbol); the
// `Opt.Some(null)` shape exercises its (b) leg (CallExpr where the borrow-
// carrying payload is a literal `NullLiteralExpr`).
// ===========================================================================
BEZEL_TEST(choice_borrow_field_2209, payload_free_case_field_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public v: Opt; "
        "  public constructor() v := Opt.None; end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var h := new Holder(); "
        "  return 0; "
        "end Main; end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(choice_borrow_field_2209, explicit_null_payload_field_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public v: Opt; "
        "  public constructor() v := Opt.Some(null); end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var h := new Holder(); "
        "  return 0; "
        "end Main; end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The same null-source exception through the field-INITIALIZER form. A class
// with `v: Opt := Opt.None;` is admitted; both the `Opt.None` (payload-free
// case) and `Opt.Some(null)` (literal-null payload) shapes must stay clean.
BEZEL_TEST(choice_borrow_field_2209, field_initializer_null_source_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class D "
        "  public v: Opt := Opt.None; "
        "end D;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(choice_borrow_field_2209,
           field_initializer_null_payload_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class D "
        "  public v: Opt := Opt.Some(null); "
        "end D;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// POSITIVE — a value-like choice stored into a class instance field is
// ordinary value-storage, NOT a borrow store (§35.4.3: every payload is
// value-typed or runtime-managed, so the choice is not borrow-bearing).
// The new `isBorrowBearingChoice` gate on the choice leg must mirror exactly
// the gate the existing `isBorrowBearingRecord` predicate applies to record
// values, and the underlying `common::classifyChoiceType` authority must
// classify `VOpt` as `ValueLike` (NOT `BorrowBearing`).
// ===========================================================================
BEZEL_TEST(choice_borrow_field_2209, value_like_choice_field_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public v: VOpt; "
        "  public constructor(x: VOpt) v := x; end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var h := new Holder(VOpt.Some(7)); "
        "  return 0; "
        "end Main; end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// POSITIVE — a borrow-bearing choice held in a method-LOCAL var is NOT a
// field-storage position. The §9.7 instance-field bullet is scoped to
// storage that outlives the source; a stack-local does not outlive the stack
// frame, so neither `checkBorrowStoreEscape` (`dest == Dest::Field` requires
// a MemberAccess / bare-field target, not an IdentifierExpr) nor
// `checkFieldInitializer` (instance-field initializer only) fires. The new
// choice leg must NOT widen to non-field positions — pinning this prevents
// over-rejection of ordinary `var x := Choice.Borrowed(local);` use sites.
// ===========================================================================
BEZEL_TEST(choice_borrow_field_2209,
           method_local_borrow_bearing_choice_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C public static method Main(): int "
        "  using var local := new Res(); "
        "  var o := Opt.Some(local); "
        "  case o of "
        "    Some(var x) => do return 1; end, "
        "    None => do return 0; end "
        "  end; "
        "end Main; end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// NO-REGRESSION — the prior #2159 face (a record WITH a borrow-bearing choice
// field is itself borrow-bearing by §9.7 transitivity) must STILL be rejected
// at the field-store site, with the record-flavoured diagnostic — NOT the
// new choice-flavoured one. This pins the cross-check that #2159's Choice
// leg of the record-field walk is still in place: without it, `HolderC`
// would be misclassified and the rule would never fire, regressing the
// #2159 fix silently.
// ===========================================================================
BEZEL_TEST(choice_borrow_field_2209,
           record_with_choice_field_still_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "choice RefC Some(r: Res), None; end RefC; "
        "record HolderC(K: RefC); "
        "class Box "
        "  public r: HolderC; "
        "  public constructor(x: HolderC) r := x; end constructor; "
        "end Box; "
        "class C public static method Main(): int return 0; end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The record-flavoured message fires (not the new choice-flavoured one):
    // the value stored has record type, and the new check applies the record
    // leg FIRST, before the choice leg.
    BEZEL_EXPECT_TRUE(hasError(
        r, "a borrow-bearing record may not be stored in an instance "
           "field of a class"));
    BEZEL_EXPECT_TRUE(hasError(r, "§9.7"));
    // The new choice-flavoured message MUST NOT appear for this shape (the
    // stored value's static type is `HolderC`, a record, not a choice).
    BEZEL_EXPECT_FALSE(hasError(
        r, "a borrow-bearing choice may not be stored in an instance "
           "field of a class"));
}

// ===========================================================================
// AOT — the full AOT+ASan path (the strict oracle) over the rejection shape.
// Without the fix, the program compiled and ASan-cleanly reached the field
// store, then read a dangling borrow on the next method call after `local`'s
// auto-drop (the §9.7 use-after-dispose the fix closes). With the fix the
// REJECT diagnostic fires at parse-and-check time and the program never
// reaches codegen, so we exercise the ACCEPT shape (a null-source choice in
// a static field) under the AOT oracle to confirm no over-rejection has
// regressed the working paths: a static-readonly `Opt` field set to
// `Opt.None` is a valid §9.7 sentinel-value (the record rule's
// `CancellationToken.None` precedent, applied to a choice-typed field).
// ===========================================================================
BEZEL_TEST(choice_borrow_field_2209, aot_static_readonly_sentinel_accepted) {
    std::string src =
        "module M; "
        "choice Opt Some(r: int), None; end Opt; "
        "class C "
        "  public static readonly S: Opt := Opt.None; "
        "  public static method Main(): int "
        "    case C.S of "
        "      Some(var n) => do return n; end, "
        "      None => do return 42; end "
        "    end; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
