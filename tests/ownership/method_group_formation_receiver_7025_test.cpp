// fix #7025 (follow-up, §27.6 / §28.6, v0.343): §28.6's receiver-ownership
// rule is read AT A §27.6 FORMATION of `Dispose`.
//
// THE RULE. §27.6 "Method references": "every receiver-side rule of the call
// the formation resolves as is read at the formation", and §28.6 "Who may call
// `Dispose()`": "`Dispose()` or a direct `DisposeAsync()` call through a
// **borrow** is a compile error. The borrow receivers: a borrowed local or
// parameter — including `this` … — a non-`owned` field read, an owned-field
// read outside the declaring class, …", and "The owned-receiver rule is
// exception-free: **only owners dispose**".
//
// THE DEFECT. The compiler had the §28.6 diagnostic only at the CALL site
// (`handleDischarge` → `checkDisposeReceiverOwnership`). The FORMATION
// spelling — `var f := d.Dispose;` — admitted every receiver the CALL
// spelling rejects. MEASURED by the reviewer: `method Take(d: D): int
// var f := d.Dispose;  f();` ⇒ the destructor ran inside `Take`, i.e. the
// CALLEE disposed the CALLER's object; with an `owned` field the caller's
// later read of the object silently succeeded on the disposed value.
//
// So the two spellings of one member disagreed about one receiver, which is
// what §27.6's first sentence forbids.
//
// THE ROWS BELOW are the reviewer's FORMATION-ONLY probes (they contain no
// `Dispose()` CALL, so nothing but the formation can reject them):
//
//   * `this` — a §28.6 borrow receiver;
//   * a borrowed (non-`owned`) PARAMETER — a §28.6 borrow receiver;
//   * a non-`owned` FIELD READ — a §28.6 borrow receiver;
//   * an `owned` FIELD READ OUTSIDE the declaring class — a §28.6 borrow
//     receiver;
//
// and the CONTROLS, which §28.6 admits and which must stay accepted:
// an `owned` parameter, an owned local, and `this.<owned field>`.
//
// Each rejection row asserts the SPECIFIC §28.6 diagnostic (not merely
// "there was an error"), so the row cannot pass on an unrelated rule.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// §28.6's own diagnostic wording — the FORMATION spelling must give the
// SAME diagnostic the CALL spelling gives, so the two spellings of one
// member cannot disagree about one receiver.
constexpr char kThisNeedle[] = "cannot call `Dispose()` on `this`";
constexpr char kParamNeedle[] = "a non-`owned` parameter is a borrow";
constexpr char kFieldNeedle[] = "cannot call `Dispose()` through a field read";

// A class with an observable destructor, so the programs below are the
// reviewer's own shapes.
constexpr char kDClass[] =
    "module M; "
    "class D "
    "  public constructor() end constructor; "
    "  destructor Console.WriteLine(\"disposed\"); end destructor; "
    "end D; ";

}  // namespace

// =============================================================================
// NEGATIVE — a FORMATION of `Dispose` over a §28.6 borrow receiver is
// rejected with the call spelling's diagnostic.
// =============================================================================

// `this` is a §28.6 borrow receiver: "an object may not dispose itself out
// from under its owner".
//
// PRE-FIX: accepted (and degraded — the #2434 zero-initialised slot), so the
// destructor never ran at all.
BEZEL_TEST(method_group_formation_receiver_7025,
           spec_27_6_formation_of_dispose_on_this_is_rejected_28_6) {
    auto r = parseAndCheckOwnership(
        std::string(kDClass) +
        "class C "
        "  public owned d: D; "
        "  public constructor() d := new D(); end constructor; "
        "  public method Go(): int "
        "    var f := this.Dispose; "
        "    return 0; "
        "  end Go; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kThisNeedle));
}

// A borrowed (non-`owned`) PARAMETER is a §28.6 borrow receiver — "a
// non-`owned` parameter is a borrow (§28.6)".
//
// PRE-FIX: accepted; INVOKING the group disposed the CALLER's object (the
// reviewer's measurement).
BEZEL_TEST(method_group_formation_receiver_7025,
           spec_27_6_formation_of_dispose_on_a_borrowed_parameter_is_rejected_28_6) {
    auto r = parseAndCheckOwnership(
        std::string(kDClass) +
        "class C "
        "  public static method Take(d: D): int "
        "    var f := d.Dispose; "
        "    return 0; "
        "  end Take; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kParamNeedle));
}

// A non-`owned` FIELD READ is a §28.6 borrow receiver — "a non-`owned` field
// is always a borrow, and an `owned` field is disposable only through `this`".
BEZEL_TEST(method_group_formation_receiver_7025,
           spec_27_6_formation_of_dispose_through_a_non_owned_field_read_is_rejected_28_6) {
    auto r = parseAndCheckOwnership(
        std::string(kDClass) +
        "class C "
        "  private _d: D; "
        "  public constructor(d: D) _d := d; end constructor; "
        "  public method Go(): int "
        "    var f := this._d.Dispose; "
        "    return 0; "
        "  end Go; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFieldNeedle));
}

// An `owned` FIELD READ OUTSIDE THE DECLARING CLASS is a §28.6 borrow
// receiver — the field's actual owner disposes it.
BEZEL_TEST(method_group_formation_receiver_7025,
           spec_27_6_formation_of_dispose_through_an_owned_field_read_outside_the_declaring_class_is_rejected_28_6) {
    auto r = parseAndCheckOwnership(
        std::string(kDClass) +
        "class Holder "
        "  public owned d: D; "
        "  public constructor() d := new D(); end constructor; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    var f := h.d.Dispose; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFieldNeedle));
}

// =============================================================================
// POSITIVE — the receivers §28.6 ADMITS must stay admitted, so the formation
// check does not over-reject.
// =============================================================================

// An `owned` PARAMETER is an owner — "an `owned`/`out owned` parameter IS
// owned — admit".
BEZEL_TEST(method_group_formation_receiver_7025,
           spec_27_6_formation_of_dispose_on_an_owned_parameter_is_admitted_28_6) {
    auto r = parseAndCheckOwnership(
        std::string(kDClass) +
        "class C "
        "  public static method Take(d: owned D): int "
        "    var f := d.Dispose; "
        "    return 0; "
        "  end Take; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An owned LOCAL is an owner — §28.6 admits "an owned local".
BEZEL_TEST(method_group_formation_receiver_7025,
           spec_27_6_formation_of_dispose_on_an_owned_local_is_admitted_28_6) {
    auto r = parseAndCheckOwnership(
        std::string(kDClass) +
        "class C "
        "  public static method Main(): int "
        "    var d := new D(); "
        "    var f := d.Dispose; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// `this.<owned field>` is the admitted §28.7 dispose-then-assign / #1230
// destructor idiom: the object may dispose its own owned field.
BEZEL_TEST(method_group_formation_receiver_7025,
           spec_27_6_formation_of_dispose_on_this_owned_field_is_admitted_28_6) {
    auto r = parseAndCheckOwnership(
        std::string(kDClass) +
        "class C "
        "  public owned d: D; "
        "  public constructor() d := new D(); end constructor; "
        "  public method Go(): int "
        "    var f := this.d.Dispose; "
        "    return 0; "
        "  end Go; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
