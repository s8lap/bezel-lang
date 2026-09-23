// Spec-derived regression tests for issue #4679 — two §9.7 borrow-lifetime
// bullets that the builtin span family escaped.
//
// §9.7 (v0.307) states the rules this file pins:
//
//   bullet 1 — "A borrow-bearing record value may not outlive the source of
//               any borrow it contains."
//   bullet 2 — "A borrow-bearing record may not be constructed from a
//               temporary bound to one of its borrow-carrying fields. A
//               construction argument that is a temporary — a collection or
//               array literal, or a fresh call result that is not bound to a
//               named location — has a lifetime that ends at the construction
//               expression, so the record would immediately outlive that
//               borrow's source (a dangling borrow)."
//
// `Span<T>` / `ReadOnlySpan<T>` (stdlib §4.14) and `ArraySpan<T>` /
// `ReadOnlyArraySpan<T>` (stdlib §4.15) are borrow-bearing records by §9.7's
// own definition — each declares a non-nullable `Buffer: OwningBuffer<T>` /
// `Source: Array<T>` — and §9.7 names a span's buffer window as the canonical
// case of the bullet. But a builtin span view has no user Record `TypeDecl`,
// which is the blind spot #1251 (field store), #2308 (collection store) and
// #1966 (return escape) each had to close separately. The two sites here are
// the remaining ones:
//
//   (A) the OUTER-LOCAL store.  `checkBorrowStoreEscape`'s plain-local channel
//       decides through `ownedLocalBorrowSource`, which DECLINES a borrow-
//       bearing record / span RHS on purpose ("those keep their existing
//       type-based store handling untouched"). For a FIELD target that
//       handling exists; for a plain local there was none, so the assignment
//       fell through every leg. MEASURED on the parent (2313ba78,
//       `--sanitize=address`): the l4_14_0001 shape compiles rc=0 with no
//       diagnostic and dies with `AddressSanitizer: SEGV ... READ memory
//       access` in the span read. The byte-identical CLOSURE spelling has
//       been rejected since #861 through the very depth comparison the fix
//       reuses.
//
//   (B) the no-temporary-source CONSTRUCTION.  `checkRecordFromTemporary`
//       gated on `rt->declaration` + `TypeDeclKind::Record`, so
//       `new Span<int>(new OwningBuffer<int>(3), 0, 3)` returned before
//       checking anything while §9.7's own worked example over a user record
//       (`new Bag([10, 20, 30])`) has been rejected since #1066.
//
// Corpus rows pinned here: l4_14_0001, l4_14_0003 (stdlib §4.14) and
// l4_15_0003 (stdlib §4.15).
//
// The REJECT rows assert the SPECIFIC diagnostic (`hasErrorMessage` with a
// unique sentence fragment, not a bare `hasErrors()` and not a §-number that
// a neighbouring rule's message could also carry — #4716's vacuous-oracle
// shape). The ACCEPT rows RUN the program through `runAotChecked` and assert
// its exit value, because a rule that newly REJECTS is exactly the rule whose
// risk is over-rejection, and a compile-only control cannot see a silently
// changed value.

#include <string>

#include "support/aot_support.hpp"        // runAotChecked — AOT + ASan/LSan
#include "support/ownership_support.hpp"  // parseAndCheckOwnership
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// The unique fragments of the two diagnostics under test. Deliberately a
// SENTENCE fragment rather than "§9.7": several neighbouring §9.7 legs
// (instance-field store, collection store, return escape) also cite §9.7, and
// a needle any of them could satisfy would pass vacuously if an earlier check
// started firing on the same input.
constexpr const char* kOutlivesNeedle =
    "so the span would outlive the backing store it views";
constexpr const char* kRecordOutlivesNeedle =
    "a borrow-bearing record may not outlive the source of any borrow it "
    "contains";
constexpr const char* kTemporaryNeedle =
    "a span may not be constructed from a temporary backing store";

}  // namespace

// ===========================================================================
// REJECT — §9.7 bullet 1, the OUTER-LOCAL store (corpus l4_14_0001).
// ===========================================================================

// stdlib §4.14 / §9.7 bullet 1: the corpus case l4_14_0001 verbatim — `s` is
// declared in the method block, `buf` in the `if` block, and `s := buf.AsSpan()`
// leaves `s` viewing a buffer whose destructor runs at the end of the inner
// block.
BEZEL_TEST(own_span_borrow_lifetime_4679,
           span_assigned_to_outer_local_rejected_9_7_l4_14_0001) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Program "
        "  public method Demo(flag: bool) "
        "    var s: Span<int>; "
        "    if flag then "
        "      using var buf := new OwningBuffer<int>(3); "
        "      s := buf.AsSpan(); "
        "    end; "
        "  end Demo; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kOutlivesNeedle));
}

// stdlib §4.14 / §9.7 bullet 1: the ONE-HOP form — the span is bound to an
// inner local first, then that local is assigned out. The provenance hop goes
// through `recordBorrowLocalSource_` (the #1726 freeze pre-pass), exactly as
// #1966's indirect return escape does.
BEZEL_TEST(own_span_borrow_lifetime_4679,
           span_local_alias_assigned_to_outer_local_rejected_9_7) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Program "
        "  public method Demo(flag: bool) "
        "    var s: Span<int>; "
        "    if flag then "
        "      using var buf := new OwningBuffer<int>(3); "
        "      var v := buf.AsSpan(); "
        "      s := v; "
        "    end; "
        "  end Demo; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kOutlivesNeedle));
}

// stdlib §4.15 / §9.7 bullet 1: the array-backed sibling, through a LOOP body
// (the loop-carried back-edge shape #861's own comment names) and a
// `ReadOnlySpan`-free `ArraySpan<T>` receiver.
BEZEL_TEST(own_span_borrow_lifetime_4679,
           array_span_assigned_out_of_loop_body_rejected_9_7) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); a.Add(1); "
        "    var s := a.AsSpan(); "
        "    var i := 0; "
        "    while i < 1 do "
        "      using var inner := new Array<int>(); inner.Add(9); "
        "      s := inner.AsSpan(); "
        "      i := i + 1; "
        "    end; "
        "    return s[0]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kOutlivesNeedle));
}

// stdlib §4.14 / §9.7 bullet 1: the READ-ONLY view face — `AsReadOnlySpan()`
// carries the same borrow of the same buffer.
BEZEL_TEST(own_span_borrow_lifetime_4679,
           readonly_span_assigned_to_outer_local_rejected_9_7) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(3, lambda(i: int): int => i + 1); "
        "    var s := buf.AsReadOnlySpan(); "
        "    if s.Length > 0 then "
        "      using var other := new OwningBuffer<int>(2, lambda(i: int): int => 9); "
        "      s := other.AsReadOnlySpan(); "
        "    end; "
        "    return s[0]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kOutlivesNeedle));
}

// §9.7 bullet 1, USER-RECORD face: the same leg serves a user borrow-bearing
// record, which is the shape the diagnostic's non-span wording is for. Pins
// that the fix is the §9.7 rule and not a span-only special case.
BEZEL_TEST(own_span_borrow_lifetime_4679,
           user_borrow_bearing_record_assigned_to_outer_local_rejected_9_7) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class R public constructor() end constructor; "
        "  public method V(): int => 1; end R; "
        "record Holder(Ref: R) end Holder; "
        "class C "
        "  public static method Main(): int "
        "    using var seed := new R(); "
        "    var h := new Holder(seed); "
        "    if h.Ref.V() > 0 then "
        "      using var inner := new R(); "
        "      h := new Holder(inner); "
        "    end; "
        "    return h.Ref.V(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kRecordOutlivesNeedle));
}

// ===========================================================================
// REJECT — §9.7 bullet 2, no-temporary-source (corpus l4_14_0003, l4_15_0003).
// ===========================================================================

// stdlib §4.14 / §9.7 bullet 2: the corpus case l4_14_0003 verbatim — a fresh
// `new OwningBuffer<int>(3)` that is not bound to a named location.
BEZEL_TEST(own_span_borrow_lifetime_4679,
           span_from_fresh_buffer_temporary_rejected_9_7_l4_14_0003) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Program "
        "  public static method Main() "
        "    var s := new Span<int>(new OwningBuffer<int>(3), 0, 3); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kTemporaryNeedle));
}

// stdlib §4.15 / §9.7 bullet 2: the corpus case l4_15_0003 verbatim — a
// collection literal, which is §9.7's own named temporary ("a collection or
// array literal") and the shape of its worked `new Bag([10, 20, 30])` example.
BEZEL_TEST(own_span_borrow_lifetime_4679,
           array_span_from_collection_literal_rejected_9_7_l4_15_0003) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Program "
        "  public static method Main() "
        "    var s := new ArraySpan<int>([1, 2, 3], 0, 3); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kTemporaryNeedle));
}

// stdlib §4.14 / §9.7 bullet 2: the ASSIGNMENT binding context (the rule is
// hooked at all three of local-decl / assignment / return, so each is pinned —
// #1066's own three call sites).
BEZEL_TEST(own_span_borrow_lifetime_4679,
           span_from_temporary_in_assignment_context_rejected_9_7) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(3); "
        "    var s := new Span<int>(buf, 0, 3); "
        "    s := new Span<int>(new OwningBuffer<int>(3), 0, 3); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kTemporaryNeedle));
}

// stdlib §4.15 / §9.7 bullet 2: the RETURN binding context.
BEZEL_TEST(own_span_borrow_lifetime_4679,
           span_from_temporary_in_return_context_rejected_9_7) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method View(): ArraySpan<int> "
        "    return new ArraySpan<int>([1, 2, 3], 0, 3); "
        "  end View; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kTemporaryNeedle));
}

// ===========================================================================
// ACCEPT — the legal shapes must still COMPILE AND RUN (no over-rejection).
// ===========================================================================

// stdlib §4.14 / §9.7: the corpus case l4_14_0004 shape — the remedy §9.7
// names for bullet 2 ("bind the source to a named local whose scope outlives
// the record, and pass that"). buf holds 1,2,3; s[2] == 3.
BEZEL_TEST(own_span_borrow_lifetime_4679,
           span_from_named_bound_buffer_accepted_and_runs) {
    auto r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(3, lambda(i: int): int => i + 1); "
        "    var s := new Span<int>(buf, 0, 3); "
        "    return s[2]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// stdlib §4.15 / §9.7: the SAME-DEPTH rebind — target and source both at
// method scope, so the target dies no later than the source. This is the
// shape own_span_field_store_1251 and own_consume_site_freeze_span_reassign
// already pin at compile level; here it must also produce the right value.
// a == [4,5,6]; sp := a.AsSpan(1) → [5,6]; sp[0] == 5.
BEZEL_TEST(own_span_borrow_lifetime_4679,
           same_depth_span_rebind_accepted_and_runs) {
    auto r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); a.Add(4); a.Add(5); a.Add(6); "
        "    var sp := a.AsSpan(); "
        "    sp := a.AsSpan(1); "
        "    return sp[0]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// stdlib §4.14 / §9.7: the SAFE nesting direction — the backing buffer is
// declared in the OUTER block and the inner block only re-derives a view. The
// target's declaring block does NOT strictly enclose the source's, so the rule
// must not fire. buf holds 1,2,3; s := buf.AsSpan(1) → [2,3]; s[0] == 2.
BEZEL_TEST(own_span_borrow_lifetime_4679,
           inner_block_view_of_outer_buffer_accepted_and_runs) {
    auto r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(3, lambda(i: int): int => i + 1); "
        "    var s := buf.AsSpan(); "
        "    if s.Length > 0 then "
        "      var inner := buf.AsSpan(1); "
        "      s := inner; "
        "    end; "
        "    return s[0]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// §22.5 / §22.10 named arguments: the no-temporary-source leg classifies each
// argument by its OWN static type, never by position, so the named spelling
// (pinned by own_named_args_method_arm_gate_3785) stays accepted — `Buffer`
// is a named local, `Start` / `Length` are primitives. buf holds 1..4;
// s[3] == 4.
BEZEL_TEST(own_span_borrow_lifetime_4679,
           span_named_ctor_args_over_bound_buffer_accepted_and_runs) {
    auto r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(4, lambda(i: int): int => i + 1); "
        "    var s := new Span<int>(Buffer := buf, Start := 0, Length := 4); "
        "    return s[3]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}

// The two value-shaped-builtin controls own_value_builtin_new_3134 pins:
// `new ArraySpan<int>(0, 0, 0)` (primitive arguments — never borrow-carrying,
// so the temporary leg must not fire on the literal `0`) and
// `new ReadOnlyStringSpan("abc", 0, 2)` (string-backed, outside the
// borrow-bearing span family entirely per §9.7's closing paragraph).
BEZEL_TEST(own_span_borrow_lifetime_4679,
           value_shaped_builtin_span_ctors_stay_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Main(): int "
        "    var a := new ArraySpan<int>(0, 0, 0); "
        "    var t := new ReadOnlyStringSpan(\"abc\", 0, 2); "
        "    return 3; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
