// Spec-derived tests for issue #1251 — §9.7 "Records and disposal" (v0.226):
// the builtin-SPAN face of the instance-field storage rule.
//
// The normative sentence (§9.7, third storage bullet):
//
//   "A borrow-bearing record value may not be stored in an **instance field of
//    a class** … A class instance's lifetime is not bounded by any borrow
//    source's scope, and a record-carried borrow — unlike a **direct**
//    class-typed borrow field, whose every use is a member access through the
//    reference and therefore hits the disposed-flag check (§28.3's field-store
//    rule, which is unchanged) — may be consumed through access paths with **no
//    per-access disposed-flag check** (a span's buffer window is the canonical
//    case). The checked-mode trust regime is therefore unavailable and the
//    store is **rejected statically**."
//
// That is the sentence which makes the rejected shapes below unsound: the
// stdlib declares `Span<T>` / `ReadOnlySpan<T>` (stdlib §4.14) and
// `ArraySpan<T>` / `ReadOnlyArraySpan<T>` (stdlib §4.15) as records over a
// class-typed backing store (`Buffer: OwningBuffer<T>` / `Source: Array<T>`),
// so a span IS a borrow-bearing record by §9.7's own definition — and §9.7
// names a span's buffer window as the CANONICAL case of this very bullet.
// A span stored in a class instance field therefore outlives its backing
// store with no disposed-flag check on the read path: verified before the fix
// as `rc=0` at compile time and SIGSEGV under ASan at run time (a
// `bz_array_get` read through the freed buffer).
//
// Before the fix, `checkBorrowStoreEscape`'s `Dest::Field` leg (and its
// field-initializer counterpart `checkFieldInitializer`) consulted only
// `isBorrowBearingRecord`, which requires a user Record `TypeDecl`. A builtin
// span has none, so a BARE span RHS matched neither leg and the store compiled
// clean. (A user record that merely CONTAINS a span field was already covered
// — #2127 taught `recordFieldIsBorrowCarrying` about span fields.)
//
// The accept-controls pin the two boundaries this rejection must not cross:
//
//   * §28.3's field-store rule for a **direct** class-typed borrow field is
//     unchanged and stays holder-kind-conditioned (parameter-/receiver-rooted
//     admissible) — §9.7 says so in the sentence quoted above.
//   * §9.7's bullet is scoped to an instance field of a **class**. A RECORD's
//     own borrow-carrying component is what §9.7's transitivity clause defines
//     as *making* the record borrow-bearing, so a body record's constructor
//     storing into its own field is legal — the positional form's synthesized
//     primary constructor (§9.1) performs exactly that store.
//   * `ReadOnlyStringSpan` views a runtime-managed `string`, which §9.7
//     excludes from borrow-bearing ("runtime-managed values have unbounded
//     lifetime by construction"), so it stays field-storable.
//
// // per §9.7, §9.1, §28.3; stdlib §4.14, §4.15 (v0.226, issue #1251)

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// True when an ERROR diagnostic whose message contains `sub` was emitted.
// Every reject below asserts the §9.7 diagnostic TEXT, not merely that the
// check failed — a different (or crashing) rejection must not satisfy it.
bool hasError(const OwnershipResult& r, const std::string& sub) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(sub) != std::string::npos) return true;
    }
    return false;
}

// The §9.7 span rejection's leading clause, at both syntactic positions.
constexpr const char* kSpanAssign =
    "a span may not be stored in an instance field of a class";
constexpr const char* kSpanInit = "a span may not be stored in the instance field";

// The remedy the diagnostic must name (§9.7: "store the source … and re-derive
// the record view at the use site").
constexpr const char* kSpanRemedy = "re-derive the span with `AsSpan()`";

}  // namespace

// =============================================================================
// NEGATIVE — a builtin span stored into a CLASS instance field.
// =============================================================================

// The issue's literal repro: a constructor stores its `ReadOnlySpan<int>`
// parameter into an instance field. Rejected unconditionally — §9.7 draws no
// distinction between a provably-safe source and an unprovable one for a
// record-carried borrow ("no trust residual"), and a span is a record over its
// backing `OwningBuffer<T>` (stdlib §4.14).
BEZEL_TEST(span_field_store_1251, constructor_param_into_field_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box "
        "  public f: ReadOnlySpan<int>; "
        "  public constructor(s: ReadOnlySpan<int>) f := s; end constructor; "
        "end Box; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kSpanAssign));
    BEZEL_EXPECT_TRUE(hasError(r, "§9.7"));
    BEZEL_EXPECT_TRUE(hasError(r, kSpanRemedy));
}

// The runtime-unsound shape the fix exists for: a span of a scope-bound
// `using var` buffer is stored into a field of a PARAMETER object that
// outlives the buffer. Before the fix this compiled clean and read the freed
// buffer in the caller (ASan: SEGV in `bz_array_get`).
BEZEL_TEST(span_field_store_1251, method_body_store_of_local_backed_span_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box "
        "  public f: Span<int>; "
        "  public constructor() end constructor; "
        "  public method Read(): int return f[0]; end Read; "
        "end Box; "
        "class C "
        "  static method Fill(box: Box) "
        "    using var buf := new OwningBuffer<int>(4); "
        "    buf[0] := 42; "
        "    box.f := buf.AsSpan(); "
        "  end Fill; "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    Fill(b); "
        "    return b.Read(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kSpanAssign));
    BEZEL_EXPECT_TRUE(hasError(r, "§4.14/§4.15"));
}

// The Array-backed span family (stdlib §4.15) is the same rule — the predicate
// must name all four builtin span heads, not just the buffer-backed pair.
BEZEL_TEST(span_field_store_1251, array_span_into_field_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box "
        "  public f: ArraySpan<int>; "
        "  public constructor(s: ArraySpan<int>) f := s; end constructor; "
        "end Box; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kSpanAssign));
}

BEZEL_TEST(span_field_store_1251, readonly_array_span_into_field_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box "
        "  public f: ReadOnlyArraySpan<int>; "
        "  public constructor(s: ReadOnlyArraySpan<int>) f := s; "
        "  end constructor; "
        "end Box; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kSpanAssign));
}

// The qualified `this.f := …` store in an ordinary (non-constructor) method —
// the same rule at the other spelling of a field write.
BEZEL_TEST(span_field_store_1251, this_qualified_store_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box "
        "  public f: ArraySpan<int>; "
        "  public constructor(s: ArraySpan<int>) f := s; end constructor; "
        "  public method Rebind(s: ArraySpan<int>) this.f := s; end Rebind; "
        "end Box; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kSpanAssign));
}

// The field-INITIALIZER position (`f: ArraySpan<int> := …` in the class body)
// is the same §9.7 rule at a different syntactic site (checkFieldInitializer),
// mirroring the #1540 record test's initializer case. The message names the
// field.
BEZEL_TEST(span_field_store_1251, field_initializer_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Src public static readonly a: Array<int> := new Array<int>(); "
        "end Src; "
        "class Box public f: ArraySpan<int> := Src.a.AsSpan(); end Box; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kSpanInit));
    BEZEL_EXPECT_TRUE(hasError(r, "§9.7"));
}

// =============================================================================
// ACCEPT-CONTROLS — the admissible side of the boundary. Each of these
// compiles both before and after the fix; they pin what the new rejection must
// NOT reach.
// =============================================================================

// §28.3's DIRECT class-typed borrow field is holder-kind-conditioned and
// explicitly "unchanged" by §9.7's bullet: a parameter-rooted borrow stored in
// a field is admissible (the §28.5 back-reference idiom). A span rejection that
// leaked into this leg would break the idiom.
BEZEL_TEST(span_field_store_1251, direct_class_borrow_field_still_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  public method Val(): int return 5; end Val; "
        "  destructor end destructor; end Res; "
        "class BackRef "
        "  public r: Res; "
        "  public constructor(x: Res) r := x; end constructor; "
        "  public method Rebind(x: Res) this.r := x; end Rebind; "
        "end BackRef; "
        "class C public static method Main(): int "
        "  using var w := new Res(); "
        "  using var b := new BackRef(w); "
        "  b.Rebind(w); "
        "  return b.r.Val(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.7: "Reference-typed fields whose types are runtime-managed (`string`, …)
// do not make the record borrow-bearing." `ReadOnlyStringSpan` (stdlib §4.13)
// views a `string`, so it is NOT a borrow-bearing span and stays storable.
BEZEL_TEST(span_field_store_1251, string_backed_span_field_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class StrView "
        "  public v: ReadOnlyStringSpan; "
        "  public constructor(s: ReadOnlyStringSpan) v := s; end constructor; "
        "end StrView; "
        "class C public static method Main(): int "
        "  using var sv := new StrView(\"hello\".AsSpan()); "
        "  return sv.v.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.7's storage bullet is scoped to an instance field of a **CLASS**. A body
// record's constructor storing a span into its OWN component is legal — §9.7's
// transitivity clause counts a span field as borrow-carrying, which is exactly
// what makes such a record a borrow-bearing record, and §9.1's synthesized
// primary constructor for the positional spelling performs the same store.
// Rejecting it would make a borrow-bearing body record unconstructible.
BEZEL_TEST(span_field_store_1251, record_own_span_component_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record SpanBoxA "
        "  public v: ArraySpan<int>; "
        "  public constructor(x: ArraySpan<int>) v := x; end constructor; "
        "end SpanBoxA; "
        "record SpanBoxB "
        "  public readonly property V: ArraySpan<int>; "
        "  public constructor(x: ArraySpan<int>) this.V := x; end constructor; "
        "end SpanBoxB; "
        "class C public static method Main(): int "
        "  using var arr := new Array<int>(); "
        "  arr.Add(1); "
        "  var a := new SpanBoxA(arr.AsSpan()); "
        "  var b := new SpanBoxB(arr.AsSpan()); "
        "  return a.v.Length + b.V.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The same class-scoping on the RECORD leg of the rule (§9.7 transitivity): a
// record component whose type is another borrow-bearing record is legal.
BEZEL_TEST(span_field_store_1251, record_own_borrow_bearing_component_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "record Inner(W: Res?); "
        "record Outer "
        "  public v: Inner; "
        "  public constructor(x: Inner) v := x; end constructor; "
        "end Outer; "
        "class C public static method Main(): int "
        "  using var w := new Res(); "
        "  var i := new Inner(w); "
        "  var o := new Outer(i); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// fix #4428 (§9.7, v0.301): the pre-#4428 version of this test exercised
// a parameter-rooted return (`Head(a: Array<int>): ArraySpan<int> { return
// a.AsSpan(0, 1); }`) as a positive control alongside the field-store shapes
// (`sp` reassignment, `Len(arr.AsSpan())` call). The field-store legs stay
// accepted — `sp` is a local, `arr.AsSpan()` borrows an escape-bound local,
// neither is a field store. The `Head` parameter-rooted return is REJECTED
// by the §9.7 v0.301 caller-side check, exactly as the issue pin describes.
// Split here so the field-store legs keep their clean control AND the §9.7
// v0.301 caller-side check gets its own regression pin in one place.
// // per §9.7 v0.301.
BEZEL_TEST(span_field_store_1251, span_local_call_and_param_rooted_return_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  static method Len(v: IReadOnlySpan<int>): int return v.Length; "
        "  end Len; "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(1); arr.Add(2); "
        "    var sp := arr.AsSpan(); "
        "    sp := arr.AsSpan(1); "
        "    return sp.Length + Len(arr.AsSpan()); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// fix #4428 (§9.7, v0.301): the `Head` leg from the pre-#4428 control is now
// the regression test for the parameter-rooted return rejection. A static
// method `Head(a: Array<int>): ArraySpan<int> { return a.AsSpan(0, 1); }` is
// rejected — `a` is a plain parameter of the returning method, the returned
// span is a borrow (§4.14/§4.15) of its backing `a`, and the call site has
// no way to re-establish a parameter-sourced borrow against its own actuals.
// // per §9.7, §4.14, §4.15; callee-side reportIfParameterRoot in
// Ownership_Checks.cpp's projection branch.
BEZEL_TEST(span_field_store_1251, param_rooted_return_rejected_9_7_v301) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  static method Head(a: Array<int>): ArraySpan<int> "
        "    return a.AsSpan(0, 1); "
        "  end Head; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned span is rooted at parameter `a`"));
}

// A value-typed record (no borrow-carrying field) is not borrow-bearing at all
// and stays storable in a class instance field — the "no over-rejection"
// control the #1540 record tests also carry.
BEZEL_TEST(span_field_store_1251, value_typed_record_into_class_field_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record PlainRec(X: int, Y: int); "
        "class Holder "
        "  public p: PlainRec; "
        "  public constructor(v: PlainRec) p := v; end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(new PlainRec(1, 2)); "
        "  return h.p.X; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The pre-existing RECORD leg is unchanged where it applies: a user record that
// CARRIES a span field, stored into a CLASS instance field, keeps the #1540 /
// #2127 record wording (the span leg must not shadow it).
BEZEL_TEST(span_field_store_1251, record_carrying_span_keeps_record_diagnostic) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record SpanRec(V: ArraySpan<int>, N: int); "
        "class RecHolder "
        "  public r: SpanRec; "
        "  public constructor(x: SpanRec) r := x; end constructor; "
        "end RecHolder; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(
        r, "a borrow-bearing record may not be stored in an instance field"));
    BEZEL_EXPECT_FALSE(hasError(r, kSpanAssign));
}
