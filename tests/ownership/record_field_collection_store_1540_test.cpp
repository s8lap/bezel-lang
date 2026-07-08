// Spec-derived tests for issue #1540 — §9.7 "Records and disposal" (v0.180):
// a borrow-bearing record may not be stored in a class instance field, nor
// copied into a collection that could outlive the borrow's source. Extends
// #1251 (return-escape / closure-capture) into the class-field and
// outliving-collection storage forms.
//
// §9.7 (v0.180): "A borrow-bearing record value may not be stored in an
// INSTANCE FIELD of a class ... unlike a direct class-typed borrow field,
// whose every use is a member access through the reference and therefore
// hits the disposed-flag check ... a record-carried borrow ... may be
// consumed through access paths with no per-access disposed-flag check ...
// The checked-mode trust regime is therefore unavailable and the store is
// REJECTED STATICALLY ... Stores of the null-source form (every borrow
// component null) are admitted."
//
// §9.7 (v0.180): "A borrow-bearing record value may be copied into a
// COLLECTION only when every carried borrow's source provably outlives the
// collection's own binding scope: a store into a collection that is a
// field, is reachable from one, escapes the method, or whose binding's
// scope strictly encloses a carried source's scope is rejected at the store
// site ... there is no trust residual for record-carried borrows (same
// reasoning as the instance-field bullet above)."
//
// The compiler's null-source recognizer (isNullSourceRecordExpr) is
// deliberately conservative/syntactic: it only recognizes a DIRECT
// `new Rec(null, ...)` construction at the exact store site (every argument
// bound to a borrow-carrying field a literal `null`), matching the
// reject-not-trust posture the rule documents for every other case (a
// record read back from a local/parameter/field is never treated as
// null-source here even if it happens to hold one at run time).
//
// // per §9.7 (v0.180, #1540; extends #1251)

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// True when an ERROR diagnostic whose message contains `sub` was emitted.
// Matching on the §9.7-specific wording keeps each test pinned to the rule
// under test (an unrelated diagnostic never satisfies it).
bool hasError(const OwnershipResult& r, const std::string& sub) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(sub) != std::string::npos) return true;
    }
    return false;
}

// `Res` is a disposable resource class; `RefRec` is a BORROW-BEARING record
// (its `W` field is class-typed, §9.7); `PlainRec` is a value-typed record
// (int fields only) — never borrow-bearing, the "no over-rejection" control.
const char* kCommon =
    "module M; "
    "class Res public constructor() end constructor; "
    "  destructor end destructor; end Res; "
    "record RefRec(W: Res?); "
    "record PlainRec(X: int, Y: int); ";

std::string mod(const std::string& decls) {
    return std::string(kCommon) + decls;
}

} // namespace

// =============================================================================
// NEGATIVE — a borrow-bearing record stored into a class instance field.
// =============================================================================

// The direct repro: a constructor stores its borrow-bearing-record parameter
// into an instance field. Rejected unconditionally — §9.7 draws no
// distinction between a provably-safe source and an unprovable one for a
// record-carried borrow ("no trust residual").
BEZEL_TEST(record_field_store_1540, constructor_param_into_field_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public r: RefRec; "
        "  public constructor(x: RefRec) r := x; end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var w := new Res(); "
        "  var rec := new RefRec(w); "
        "  var h := new Holder(rec); "
        "  return 0; "
        "end Main; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(
        r, "may not be stored in an instance field of a class"));
    BEZEL_EXPECT_TRUE(hasError(r, "§9.7"));
}

// The same rule reached through an ordinary (non-constructor) method's
// `this.f := …` store, not just a constructor body.
BEZEL_TEST(record_field_store_1540, method_body_assignment_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public r: RefRec; "
        "  public constructor() r := new RefRec(null); end constructor; "
        "  public method Rebind(x: RefRec) this.r := x; end Rebind; "
        "end Holder; "
        "class C public static method Fill(h: Holder, w: Res) "
        "  h.Rebind(new RefRec(w)); "
        "end Fill; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(
        r, "may not be stored in an instance field of a class"));
}

// The field-INITIALIZER form (`f: T := expr;` in the class body) is the
// same rule at a different syntactic position (checkFieldInitializer). The
// source is a static field of the SAME class (a place, not a temporary, so
// the unrelated #1066 "constructed from a temporary" rule does not also
// fire) and is not the null-source form, so the store is rejected. (A
// mutable static of class type also trips the unrelated §22.12 domain-
// safety rule here — harmless: this test only asserts the §9.7 message is
// among the diagnostics, not that it is the only one.)
BEZEL_TEST(record_field_store_1540, field_initializer_non_null_source_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder2 "
        "  public static shared: Res? := null; "
        "  public h: RefRec := new RefRec(shared); "
        "end Holder2;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "instance field `h`"));
    BEZEL_EXPECT_TRUE(hasError(
        r, "may not be stored in the instance field"));
}

// The field-initializer null-source form is admitted, mirroring the
// assignment-site exception above.
BEZEL_TEST(record_field_store_1540, field_initializer_null_source_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class D "
        "  public h: RefRec := new RefRec(null); "
        "end D;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// POSITIVE — no over-rejection at the class-field storage site.
// =============================================================================

// The null-source form — `new RefRec(null)` constructed directly at the
// store site — is explicitly admitted (§9.7's sentinel-value exception,
// e.g. `CancellationToken.None`'s shape).
BEZEL_TEST(record_field_store_1540, null_source_construction_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public r: RefRec; "
        "  public constructor() r := new RefRec(null); end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var h := new Holder(); "
        "  return 0; "
        "end Main; end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A NON-borrow-bearing record (only value-typed fields) is never subject to
// this rule at all — storing it into a field is ordinary value-copy and
// stays accepted (the critical "no over-rejection" control case).
BEZEL_TEST(record_field_store_1540, non_borrow_bearing_record_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public p: PlainRec; "
        "  public constructor(x: PlainRec) p := x; end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var pr := new PlainRec(1, 2); "
        "  var h := new Holder(pr); "
        "  return 0; "
        "end Main; end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// NEGATIVE — a borrow-bearing record copied into an outliving collection.
// =============================================================================

// The collection is a FIELD of the class — a field always outlives (and is
// reachable beyond) the storing method's scope, so the store is rejected
// unconditionally, exactly like the instance-field rule above.
BEZEL_TEST(record_collection_store_1540, field_collection_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  private owned _pool: Array<RefRec>; "
        "  public constructor() _pool := new Array<RefRec>(); end constructor; "
        "  public method Fill(w: Res) "
        "    var rec := new RefRec(w); "
        "    _pool.Add(rec); "
        "  end Fill; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(
        r, "may not be copied into a collection reachable beyond this "
           "method's scope"));
    BEZEL_EXPECT_TRUE(hasError(r, "§9.7"));
}

// The collection is a PARAMETER — caller-supplied storage that outlives
// this method call, the same "escapes the method" clause of the rule.
BEZEL_TEST(record_collection_store_1540, parameter_collection_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class C public static method Fill(pool: Array<RefRec>, w: Res) "
        "  var rec := new RefRec(w); "
        "  pool.Add(rec); "
        "end Fill; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(
        r, "may not be copied into a collection reachable beyond this "
           "method's scope"));
}

// =============================================================================
// POSITIVE — no over-rejection at the collection storage site.
// =============================================================================

// The null-source form constructed directly at the `.Add(...)` call site is
// admitted, mirroring the field-store exception.
BEZEL_TEST(record_collection_store_1540, null_source_construction_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  private owned _pool: Array<RefRec>; "
        "  public constructor() _pool := new Array<RefRec>(); end constructor; "
        "  public method Fill() "
        "    _pool.Add(new RefRec(null)); "
        "  end Fill; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A non-borrow-bearing record stored into a FIELD collection stays accepted
// — the rule never applies to it (no over-rejection).
BEZEL_TEST(record_collection_store_1540, non_borrow_bearing_record_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  private owned _pool: Array<PlainRec>; "
        "  public constructor() _pool := new Array<PlainRec>(); end constructor; "
        "  public method Fill(p: PlainRec) "
        "    _pool.Add(p); "
        "  end Fill; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A collection that is a purely LOCAL (non-escaping) binding is governed by
// the pre-existing §28.11 scope-comparison residual (unchanged by #1540,
// which only tightens the Escapes-classified receiver case) — storing a
// record built from a PARAMETER-sourced borrow into a method-scope local
// collection is not flagged.
BEZEL_TEST(record_collection_store_1540,
           local_collection_parameter_sourced_element_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C public static method Fill(w: Res): int "
        "  using var pool := new Array<RefRec>(); "
        "  pool.Add(new RefRec(w)); "
        "  return pool.Count; "
        "end Fill; end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
