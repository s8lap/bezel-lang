// fix #3973 (§9.7 "Records and disposal", v0.295): `checkStaticReadonlyRecordInit`
// (src/sema/TypeResolution_Checks.cpp) was a SHALLOW, one-level walk over a
// `static readonly` record/choice field's `new T(...)` initializer — a
// positional slot whose OWN resolved type is itself a record or choice was
// always treated as value-typed (§3 "records and choices of value types are
// values") and skipped outright, with no look at what THAT nested
// record/choice itself contains.
//
// THE DEFECT (measured on this fix's parent, `bezelc -o out repro.bzl`,
// purely LOCAL, matching the issue's own repro):
//
//     module SpanStatic2;
//     public record SpanHolder(V: ReadOnlyArraySpan<int>);
//     class Holder
//       public static readonly Src: Array<int> := new Array<int>();
//       public static readonly F: SpanHolder :=
//         new SpanHolder(new ReadOnlyArraySpan<int>(Holder.Src, 0, 0));
//     end Holder;
//     class Program
//       public static method Main(): int return 0; end Main;
//     end Program;
//
//   compiled at exit 0 with only the (correct, pre-existing) `Src`
//   declaration-site warning — `F` is a static readonly field holding a
//   LIVE borrow of `Holder.Src` through the nested `ReadOnlyArraySpan<int>`
//   view, the exact process-lifetime dangling-reference shape §9.7's
//   static-field bullet exists to reject, and got ZERO diagnostic.
//
// ── ROOT CAUSE (two independent, stacked gaps, both fixed here) ───────────
//
// (1) `checkStaticReadonlyNullSource`'s `deferred` gate (the boolean that
//     decides whether a member's initializer is even walked) consulted only
//     `isBorrowBearingRecordForStaticReadonly`, a ONE-LEVEL predicate that
//     asks whether `SpanHolder` itself has an immediately reference-typed
//     field — it does not (its one field, `V`, is Record-kind
//     `ReadOnlyArraySpan<int>`, which the same one-level test classifies as
//     value-typed). So `SpanHolder`-typed members never reached the walk at
//     all. Fixed by also OR-ing in `common::isBorrowBearingRecordOrSpan`
//     (OwnershipFacts.hpp) — the SAME predicate the declaration-site
//     `reportRule4` already uses for its own identical defer decision — which
//     performs a full transitive walk and correctly recognizes `SpanHolder`
//     as borrow-bearing via its nested span-view field.
//
// (2) Even with (1) fixed in isolation, `checkStaticReadonlyRecordInit`
//     itself still found nothing: its per-field test
//     (`staticReadonlyFieldIsReferenceTyped`) classifies a Record/Choice-kind
//     field as value-typed and the walk just `continue`s past it, never
//     looking at what THAT nested record/choice's own fields hold. Fixed by
//     recursing into a nested record/choice-typed slot's own `new T(...)`
//     initializer (when the initializer is statically that shape) and
//     re-running the identical per-slot check against its arguments.
//
// Fixing only (1) is OBSERVATIONALLY INERT (this is exactly what the issue's
// own investigation measured and recorded): the walk still finds nothing
// without (2). Both are needed together.
//
// ── SHAPE SWEEP (measured; see the commit body for exact FAIL-WITHOUT /
//    PASS-WITH counts and filters run) ─────────────────────────────────────
//   REJECT:
//     - depth-2 nesting, span-view field holding a live borrow  (headline)
//     - depth-3 nesting, same live borrow two levels further in
//   ACCEPT (the accept-side sweep §15 of the lane rules demands for any fix
//   that adds a rejection — enumerates what a `static readonly` record field
//   may legitimately contain and must keep compiling):
//     - depth-2 nested record, every component a plain value type
//     - depth-3 nested record, every component a plain value type
//     - depth-2 nested record whose OWN nested slot is a nullable class
//       correctly initialized to the §9.7 null-source form (`null`)
//     - depth-3, same null-source shape one level further in
//     - a generic record nested inside another record, instantiated at a
//       value type (int) — recursion must not choke on generic args
//     - a record nested inside another record where the nested record ALSO
//       has a plain (non-owned) inline-array-of-int field — an orthogonal
//       kind untouched by this fix (InlineArray-kind slots are not Named,
//       so the new recursive branch's own guard never engages for them)
//     - a nested slot whose initializer is NOT statically a `new T(...)`
//       (a static factory-method call) — the static analysis cannot
//       disprove null-source for it, so it is accepted exactly as the
//       existing top-level policy already accepts a non-`new` top-level
//       initializer
//
// Spec citations verified verbatim against docs/Bezel_Language_Spec.md and
// docs/Bezel_Standard_Library.md headings at v0.295: §9.7 "Records and
// disposal"; stdlib §4.14 "OwningBuffer<T>, Span<T>, ReadOnlySpan<T>" and
// §4.15 "ArraySpan<T>, ReadOnlyArraySpan<T>" (`ReadOnlyArraySpan<T>`'s
// declared shape, stdlib §4.15: `public record ReadOnlyArraySpan<T>(Source:
// Array<T>, Start: int, Length: int): IReadOnlySpan<T>` — the field the
// REJECT rows below name is `Source`, not `Src`).

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// =========================================================================
// REJECT: a nested borrow-bearing record slot must be enforced.
// =========================================================================

// REJECT (headline, depth 2): the issue's own repro, renamed for this file.
// `SpanHolder3973`'s only field is a `ReadOnlyArraySpan<int>` — a span view,
// itself a plain user-level record (stdlib §4.15) — holding a live borrow of
// `Holder3973.Src`. Must be rejected naming the nested field `Source`.
BEZEL_TEST(static_readonly_nested_borrow_bearing_record_3973,
           depth2_span_view_field_rejected_spec_9_7_issue_3973) {
    const char* prog =
        "module M3973A; "
        "public record SpanHolder3973A(V: ReadOnlyArraySpan<int>); "
        "class Holder3973A "
        "  public static readonly Src: Array<int> := new Array<int>(); "
        "  public static readonly F: SpanHolder3973A := "
        "    new SpanHolder3973A(new ReadOnlyArraySpan<int>(Holder3973A.Src, 0, 0)); "
        "end Holder3973A; "
        "class Program3973A public static method Main(): int return 0; "
        "end Main; end Program3973A;";
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "has record value with non-null reference-typed field `Source`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9.7 requires the null-source form"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`F`"));
}

// REJECT (depth 3): the same live borrow reached one level further in — a
// record wrapping the depth-2 shape above. Proves the recursion is not
// special-cased to exactly one extra level.
BEZEL_TEST(static_readonly_nested_borrow_bearing_record_3973,
           depth3_span_view_field_rejected_spec_9_7_issue_3973) {
    const char* prog =
        "module M3973B; "
        "public record SpanHolder3973B(V: ReadOnlyArraySpan<int>); "
        "public record Wrapper3973B(H: SpanHolder3973B); "
        "class Holder3973B "
        "  public static readonly Src: Array<int> := new Array<int>(); "
        "  public static readonly G: Wrapper3973B := "
        "    new Wrapper3973B(new SpanHolder3973B("
        "      new ReadOnlyArraySpan<int>(Holder3973B.Src, 0, 0))); "
        "end Holder3973B; "
        "class Program3973B public static method Main(): int return 0; "
        "end Main; end Program3973B;";
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "has record value with non-null reference-typed field `Source`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9.7 requires the null-source form"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`G`"));
}

// =========================================================================
// ACCEPT-SIDE SWEEP (§15 of the lane rules): every legitimate shape below
// must keep compiling — a false reject is worse than the bug this fixes.
// =========================================================================

// ACCEPT (depth 2, all value components): a nested record with no
// reference-typed field anywhere must compile and run cleanly.
BEZEL_TEST(static_readonly_nested_borrow_bearing_record_3973,
           depth2_plain_value_nested_record_accepted) {
    const char* prog =
        "module M3973C; "
        "record InnerVal3973C(X: int); "
        "record OuterVal3973C(I: InnerVal3973C); "
        "class Holder3973C "
        "  public static readonly F: OuterVal3973C := new OuterVal3973C(new InnerVal3973C(5)); "
        "end Holder3973C; "
        "class Program3973C public static method Main(): int return Holder3973C.F.I.X; "
        "end Main; end Program3973C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ACCEPT (depth 3, all value components): same shape, one level deeper.
BEZEL_TEST(static_readonly_nested_borrow_bearing_record_3973,
           depth3_plain_value_nested_record_accepted) {
    const char* prog =
        "module M3973D; "
        "record Level1_3973D(X: int); "
        "record Level2_3973D(A: Level1_3973D); "
        "record Level3_3973D(B: Level2_3973D); "
        "class Holder3973D "
        "  public static readonly F: Level3_3973D := "
        "    new Level3_3973D(new Level2_3973D(new Level1_3973D(9))); "
        "end Holder3973D; "
        "class Program3973D public static method Main(): int return Holder3973D.F.B.A.X; "
        "end Main; end Program3973D;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}

// ACCEPT (depth 2, nested null-source): the nested slot IS a reference-typed
// (nullable class) field, but correctly initialized to `null` — the §9.7
// null-source form — one level in. Must not be a false reject.
BEZEL_TEST(static_readonly_nested_borrow_bearing_record_3973,
           depth2_nested_nullable_class_null_source_accepted) {
    const char* prog =
        "module M3973E; "
        "class Widget3973E "
        "  public method Render(): int return 42; end Render; "
        "end Widget3973E; "
        "record Leaf3973E(W: Widget3973E?); "
        "record Mid3973E(L: Leaf3973E); "
        "class Holder3973E "
        "  public static readonly F: Mid3973E := new Mid3973E(new Leaf3973E(null)); "
        "end Holder3973E; "
        "class Program3973E public static method Main(): int return 3; "
        "end Main; end Program3973E;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ACCEPT (depth 3, nested null-source): same null-source shape two levels
// in, matching the brief's explicit "depth 2 AND 3" requirement.
BEZEL_TEST(static_readonly_nested_borrow_bearing_record_3973,
           depth3_nested_nullable_class_null_source_accepted) {
    const char* prog =
        "module M3973F; "
        "class Widget3973F "
        "  public method Render(): int return 42; end Render; "
        "end Widget3973F; "
        "record Leaf3973F(W: Widget3973F?); "
        "record Mid3973F(L: Leaf3973F); "
        "record Top3973F(M: Mid3973F); "
        "class Holder3973F "
        "  public static readonly F: Top3973F := "
        "    new Top3973F(new Mid3973F(new Leaf3973F(null))); "
        "end Holder3973F; "
        "class Program3973F public static method Main(): int return 4; "
        "end Main; end Program3973F;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}

// ACCEPT (generic nested record, value instantiation): a nested record field
// is itself a generic record instantiated at a value type. The recursion's
// own guard (Named + Record/Choice declaration) does not special-case
// generics; this pins that it does not choke on one either.
BEZEL_TEST(static_readonly_nested_borrow_bearing_record_3973,
           generic_record_nested_value_instantiation_accepted) {
    const char* prog =
        "module M3973G; "
        "record Box3973G<T>(V: T); "
        "record OuterG3973G(B: Box3973G<int>); "
        "class Holder3973G "
        "  public static readonly F: OuterG3973G := new OuterG3973G(new Box3973G<int>(7)); "
        "end Holder3973G; "
        "class Program3973G public static method Main(): int return Holder3973G.F.B.V; "
        "end Main; end Program3973G;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ACCEPT (nested record with a plain inline-array field): the nested slot's
// OWN type has an inline-array component (of a value element type) — an
// orthogonal kind (`TypeRef::Kind::InlineArray`, not `Named`) that this
// fix's new recursive branch does not touch at all (its guard only fires on
// a Named Record/Choice slot type); included as a control confirming this
// shape is undisturbed.
BEZEL_TEST(static_readonly_nested_borrow_bearing_record_3973,
           nested_record_with_inline_array_value_field_accepted) {
    const char* prog =
        "module M3973H; "
        "record ArrHolder3973H(Vals: [3]int); "
        "record OuterH3973H(A: ArrHolder3973H); "
        "class Holder3973H "
        "  public static readonly F: OuterH3973H := "
        "    new OuterH3973H(new ArrHolder3973H([1, 2, 3])); "
        "end Holder3973H; "
        "class Program3973H public static method Main(): int return Holder3973H.F.A.Vals[1]; "
        "end Main; end Program3973H;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// ACCEPT (nested non-`new` initializer): the nested slot's initializer is a
// static factory-method CALL, not statically a `new T(...)` — the static
// analysis cannot disprove null-source for it, so the recursive branch's own
// `dynamic_cast<const ast::NewExpr*>` guard does not fire and the slot is
// left alone, exactly mirroring the existing TOP-LEVEL policy for a
// non-`new` initializer (the comment directly above `checkStaticReadonlyRecordInit`'s
// definition). Must not be a false reject.
BEZEL_TEST(static_readonly_nested_borrow_bearing_record_3973,
           nested_record_non_new_initializer_accepted) {
    const char* prog =
        "module M3973I; "
        "class Widget3973I "
        "  public method Render(): int return 42; end Render; "
        "end Widget3973I; "
        "record Leaf3973I(W: Widget3973I?); "
        "record Mid3973I(L: Leaf3973I); "
        "static class Factory3973I "
        "  public static method MakeLeaf(): Leaf3973I return new Leaf3973I(null); "
        "  end MakeLeaf; "
        "end Factory3973I; "
        "class Holder3973I "
        "  public static readonly F: Mid3973I := new Mid3973I(Factory3973I.MakeLeaf()); "
        "end Holder3973I; "
        "class Program3973I public static method Main(): int return 6; "
        "end Main; end Program3973I;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}
