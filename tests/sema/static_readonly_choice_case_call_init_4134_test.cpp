// fix #4134 (§9.7 "Records and disposal", v0.295): `checkStaticReadonlyRecordInit`
// (src/sema/TypeResolution_Checks.cpp) has a Choice-kind branch that walks a
// choice value's payload arguments and reports "has choice value with
// non-null reference-typed payload" when one is non-null. That branch — and
// fix #3973's own new `checkNestedBorrowBearingSlot` recursion inside it —
// can only run when `checkStaticReadonlyRecordInit` is actually invoked with
// an `ast::NewExpr`, because BOTH of its callers gate on
// `dynamic_cast<const ast::NewExpr*>` (the top-level
// `checkStaticReadonlyNullSource`, and `checkNestedBorrowBearingSlot`'s own
// recursive dispatch).
//
// But a choice VALUE in this language is constructed with `Type.Case(args)`
// — a qualified case CALL that resolves to a `ChoiceCaseSymbol`
// (ExprTypecheck_Calls.cpp's call-typing arm) — and the spec is explicit
// that this is the ONLY construction form: a choice "has no constructors —
// a choice's construction *is* case selection" (§35.1; the field-initializer
// admission list names case construction separately from `new` expressions
// for exactly this reason). So for any SPEC-COMPLIANT static readonly choice
// initializer, the Choice arm was unreachable and §9.7 got zero enforcement.
//
// MEASURED (verified BY EXECUTION, not by reading, per this issue's review
// method — see the commit body for the `new ChoiceType(args)` finding this
// ruled out as the reachability path instead):
//
//     module MSweep7;
//     public record SpanHolderS7(V: ReadOnlyArraySpan<int>);
//     public choice ChoiceHolderS7 Has(H: SpanHolderS7), Empty(); end ChoiceHolderS7;
//     class HolderS7
//       public static readonly Src: Array<int> := new Array<int>();
//       public static readonly F: ChoiceHolderS7 :=
//         ChoiceHolderS7.Has(new SpanHolderS7(new ReadOnlyArraySpan<int>(HolderS7.Src, 0, 0)));
//     end HolderS7;
//     class ProgramS7 public static method Main(): int return 0; end Main; end ProgramS7;
//
//   compiled at exit 0 with only the (correct, pre-existing, UNRELATED)
//   `Src` declaration-site domain-confinement warning — `F` holds a live
//   borrow of `HolderS7.Src` through `SpanHolderS7`'s span-view field,
//   exactly the shape fix #3973 closed for the RECORD-typed case, with zero
//   §9.7 diagnostic for the CHOICE-typed case.
//
// ── FIX ─────────────────────────────────────────────────────────────────
// Recognize the real choice-construction spelling directly, at both call
// sites that previously gated on `NewExpr` alone: a `CallExpr` whose callee
// resolves (`SemaContext::exprSymbol`) to a `ChoiceCaseSymbol` is a case
// construction. Rather than reusing the existing NewExpr-driven Choice arm
// (which has no case identity to key on for a bare `new Choice(...)` and
// must GUESS the intended case by matching arity across every declared
// case — itself unsound when two cases share an arity), a new
// `checkStaticReadonlyChoiceCaseInit` walks the ALREADY-RESOLVED case's
// exact payload against the call's exact arguments — no guessing.
// `checkNestedBorrowBearingSlot`'s own Choice-declaration arm gets the
// identical recognition, so a NESTED choice-typed slot (a record field, or
// a choice payload, whose own type is a choice) constructed via
// `Type.Case(...)` is not silently skipped either.
//
// ── A NOTE ON THE "DEAD CODE" CLAIM (review finding, not part of the fix)
// The issue's filing believed the NewExpr-only Choice arm to be entirely
// unreachable. Verified BY EXECUTION that this is not quite true: the
// compiler currently (incorrectly, per the §35.1 prose above) ACCEPTS
// `new ChoiceType(args)` as a plain NewExpr with no "choice has no
// constructors" rejection, so that arm IS reachable through that separate,
// pre-existing bug — and constructing a `case` STATEMENT over such a value
// crashes the compiler (SIGSEGV). That defect is independent of this fix
// (this fix does not touch the `new`-expression path or attempt to reject
// it) and is filed separately; it does not change the conclusion that the
// SPEC-COMPLIANT `Type.Case(...)` spelling this test exercises got zero
// §9.7 enforcement before this fix.
//
// ── SHAPE SWEEP (§15 accept-side sweep; see commit body for FAIL-WITHOUT /
//    PASS-WITH counts and filters run) ─────────────────────────────────────
//   REJECT:
//     - depth-1 choice, directly reference-typed payload, non-null
//     - depth-2, headline repro: choice payload is a record nested with a
//       live-borrowing span view
//     - depth-3, one level further in
//     - a RECORD field whose type is itself a choice, constructed via
//       `Type.Case(...)` — exercises checkNestedBorrowBearingSlot's new arm
//     - the enclosing type is a `record`, not a `class` (§22.12's static
//       surface is class/enum-specific; §9.7 is not)
//   ACCEPT:
//     - a payload-free case (`Empty()`)
//     - a nullable reference-typed payload correctly null-sourced
//     - a plain value-typed (int) payload
//     - a nested record payload whose own reference-typed field is
//       correctly null-sourced
//     - a CallExpr initializer that is NOT a choice-case construction (an
//       ordinary static factory method returning the choice) — proves the
//       fix does not over-reject every CallExpr initializer
//     - the enclosing type is a `record`, matching the reject-side control
//
// Spec citations verified verbatim against docs/Bezel_Language_Spec.md at
// v0.295: §9.7 "Records and disposal"; §35.1 "Declaration" (anchor
// sec-choice-declaration; "a choice's construction *is* case selection" is
// this section's own §35.2 "Members" paragraph, quoted again at §22.8
// "Field initializers"); §35.3 "Construction and payload ownership".

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// =========================================================================
// REJECT: a choice-case CALL initializer must get real §9.7 enforcement.
// =========================================================================

// REJECT (depth 1): a directly reference-typed payload, non-null, via the
// real `Type.Case(args)` construction spelling.
BEZEL_TEST(static_readonly_choice_case_call_init_4134,
           depth1_direct_reference_payload_rejected_spec_9_7_issue_4134) {
    const char* prog =
        "module M4134A; "
        "class Widget4134A "
        "  public method Render(): int return 42; end Render; "
        "end Widget4134A; "
        "public choice Choice4134A Has(H: Widget4134A), Empty(); end Choice4134A; "
        "class Holder4134A "
        "  public static readonly F: Choice4134A := Choice4134A.Has(new Widget4134A()); "
        "end Holder4134A; "
        "class Program4134A public static method Main(): int return 0; "
        "end Main; end Program4134A;";
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "has choice value with non-null reference-typed payload `H`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9.7 requires the null-source form"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`F`"));
}

// REJECT (depth 2, headline repro): the issue's own measured repro, renamed
// for this file. The choice payload is a record whose own field is a
// `ReadOnlyArraySpan<int>` (a span view, stdlib §4.15) holding a live borrow
// of a sibling static field.
BEZEL_TEST(static_readonly_choice_case_call_init_4134,
           depth2_nested_span_view_in_record_payload_rejected_spec_9_7_issue_4134) {
    const char* prog =
        "module M4134B; "
        "public record SpanHolder4134B(V: ReadOnlyArraySpan<int>); "
        "public choice Choice4134B Has(H: SpanHolder4134B), Empty(); end Choice4134B; "
        "class Holder4134B "
        "  public static readonly Src: Array<int> := new Array<int>(); "
        "  public static readonly F: Choice4134B := "
        "    Choice4134B.Has(new SpanHolder4134B("
        "      new ReadOnlyArraySpan<int>(Holder4134B.Src, 0, 0))); "
        "end Holder4134B; "
        "class Program4134B public static method Main(): int return 0; "
        "end Main; end Program4134B;";
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "has record value with non-null reference-typed field `Source`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9.7 requires the null-source form"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`F`"));
}

// REJECT (depth 3): the same live borrow one level further in — a record
// wrapping the depth-2 shape above, still reached through a choice-case call.
BEZEL_TEST(static_readonly_choice_case_call_init_4134,
           depth3_nested_record_in_record_payload_rejected_spec_9_7_issue_4134) {
    const char* prog =
        "module M4134C; "
        "public record SpanHolder4134C(V: ReadOnlyArraySpan<int>); "
        "public record Outer4134C(Inner: SpanHolder4134C); "
        "public choice Choice4134C Has(H: Outer4134C), Empty(); end Choice4134C; "
        "class Holder4134C "
        "  public static readonly Src: Array<int> := new Array<int>(); "
        "  public static readonly F: Choice4134C := "
        "    Choice4134C.Has(new Outer4134C(new SpanHolder4134C("
        "      new ReadOnlyArraySpan<int>(Holder4134C.Src, 0, 0)))); "
        "end Holder4134C; "
        "class Program4134C public static method Main(): int return 0; "
        "end Main; end Program4134C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "has record value with non-null reference-typed field `Source`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9.7 requires the null-source form"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`F`"));
}

// REJECT (nested CHOICE inside a RECORD field): a record field whose OWN
// type is a choice, constructed via `Type.Case(...)` rather than `new` —
// exercises `checkNestedBorrowBearingSlot`'s new Choice-CallExpr arm, not
// the top-level dispatch.
BEZEL_TEST(static_readonly_choice_case_call_init_4134,
           nested_choice_typed_record_field_rejected_spec_9_7_issue_4134) {
    const char* prog =
        "module M4134D; "
        "public record SpanHolder4134D(V: ReadOnlyArraySpan<int>); "
        "public choice InnerChoice4134D Has(H: SpanHolder4134D), Empty(); "
        "end InnerChoice4134D; "
        "public record OuterRec4134D(Inner: InnerChoice4134D); "
        "class Holder4134D "
        "  public static readonly Src: Array<int> := new Array<int>(); "
        "  public static readonly F: OuterRec4134D := "
        "    new OuterRec4134D(InnerChoice4134D.Has(new SpanHolder4134D("
        "      new ReadOnlyArraySpan<int>(Holder4134D.Src, 0, 0)))); "
        "end Holder4134D; "
        "class Program4134D public static method Main(): int return 0; "
        "end Main; end Program4134D;";
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "has record value with non-null reference-typed field `Source`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9.7 requires the null-source form"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`F`"));
}

// REJECT (enclosing type is a RECORD, not a class): §22.12's static-and-
// domains surface is class/enum-specific, but §9.7 is not — a `record`
// declaring its own `static readonly` choice field must be enforced
// identically.
BEZEL_TEST(static_readonly_choice_case_call_init_4134,
           record_enclosing_type_direct_reference_payload_rejected_spec_9_7_issue_4134) {
    const char* prog =
        "module M4134E; "
        "class Widget4134E "
        "  public method Render(): int return 42; end Render; "
        "end Widget4134E; "
        "public choice Choice4134E Has(H: Widget4134E), Empty(); end Choice4134E; "
        "record Holder4134E "
        "  public static readonly F: Choice4134E := Choice4134E.Has(new Widget4134E()); "
        "end Holder4134E; "
        "class Program4134E public static method Main(): int return 0; "
        "end Main; end Program4134E;";
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "has choice value with non-null reference-typed payload `H`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9.7 requires the null-source form"));
}

// =========================================================================
// ACCEPT-SIDE SWEEP (§15 of the lane rules): every legitimate shape below
// must keep compiling — a false reject is worse than the bug this fixes.
// =========================================================================

// ACCEPT (payload-free case): `Empty()` has no payload at all.
BEZEL_TEST(static_readonly_choice_case_call_init_4134,
           payload_free_case_accepted) {
    const char* prog =
        "module M4134F; "
        "public record SpanHolder4134F(V: ReadOnlyArraySpan<int>); "
        "public choice Choice4134F Has(H: SpanHolder4134F), Empty(); end Choice4134F; "
        "class Holder4134F "
        "  public static readonly F: Choice4134F := Choice4134F.Empty(); "
        "end Holder4134F; "
        "class Program4134F public static method Main(): int return 11; "
        "end Main; end Program4134F;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 11);
}

// ACCEPT (nullable reference payload, null-source): the payload IS
// reference-typed, but correctly `null` — the §9.7 null-source form.
BEZEL_TEST(static_readonly_choice_case_call_init_4134,
           nullable_reference_payload_null_source_accepted) {
    const char* prog =
        "module M4134G; "
        "class Widget4134G "
        "  public method Render(): int return 42; end Render; "
        "end Widget4134G; "
        "public choice Choice4134G Has(H: Widget4134G?), Empty(); end Choice4134G; "
        "class Holder4134G "
        "  public static readonly F: Choice4134G := Choice4134G.Has(null); "
        "end Holder4134G; "
        "class Program4134G public static method Main(): int return 12; "
        "end Main; end Program4134G;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 12);
}

// ACCEPT (plain value-typed payload): an `int` payload is not
// reference-typed at all — no §9.7 obligation.
BEZEL_TEST(static_readonly_choice_case_call_init_4134,
           value_typed_payload_accepted) {
    const char* prog =
        "module M4134H; "
        "public choice Choice4134H Has(H: int), Empty(); end Choice4134H; "
        "class Holder4134H "
        "  public static readonly F: Choice4134H := Choice4134H.Has(42); "
        "end Holder4134H; "
        "class Program4134H public static method Main(): int return 13; "
        "end Main; end Program4134H;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 13);
}

// ACCEPT (nested record payload, correctly null-sourced): the record
// payload's own reference-typed field is `null` — must not be a false
// reject of the recursive walk.
BEZEL_TEST(static_readonly_choice_case_call_init_4134,
           nested_record_payload_null_source_accepted) {
    const char* prog =
        "module M4134I; "
        "class Widget4134I "
        "  public method Render(): int return 42; end Render; "
        "end Widget4134I; "
        "public record Leaf4134I(W: Widget4134I?); "
        "public choice Choice4134I Has(H: Leaf4134I), Empty(); end Choice4134I; "
        "class Holder4134I "
        "  public static readonly F: Choice4134I := Choice4134I.Has(new Leaf4134I(null)); "
        "end Holder4134I; "
        "class Program4134I public static method Main(): int return 14; "
        "end Main; end Program4134I;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 14);
}

// ACCEPT (non-case-construction CallExpr initializer): an ordinary static
// factory method call that happens to return a choice-typed value. The
// callee resolves to a plain method symbol, not a `ChoiceCaseSymbol`, so
// this fix's new recognition must not fire — proves the fix does not
// over-reject every CallExpr-initialized static readonly choice field.
BEZEL_TEST(static_readonly_choice_case_call_init_4134,
           non_case_call_initializer_accepted) {
    const char* prog =
        "module M4134J; "
        "class Widget4134J "
        "  public method Render(): int return 42; end Render; "
        "end Widget4134J; "
        "public choice Choice4134J Has(H: Widget4134J), Empty(); end Choice4134J; "
        "static class Factory4134J "
        "  public static method Build(): Choice4134J return Choice4134J.Empty(); "
        "  end Build; "
        "end Factory4134J; "
        "class Holder4134J "
        "  public static readonly F: Choice4134J := Factory4134J.Build(); "
        "end Holder4134J; "
        "class Program4134J public static method Main(): int return 15; "
        "end Main; end Program4134J;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 15);
}

// ACCEPT (enclosing type is a RECORD, correctly null-sourced): the
// reject-side control's record-enclosing-type shape, but with a correct
// null-source payload — must keep compiling.
BEZEL_TEST(static_readonly_choice_case_call_init_4134,
           record_enclosing_type_null_source_accepted) {
    const char* prog =
        "module M4134K; "
        "class Widget4134K "
        "  public method Render(): int return 42; end Render; "
        "end Widget4134K; "
        "public choice Choice4134K Has(H: Widget4134K?), Empty(); end Choice4134K; "
        "record Holder4134K "
        "  public static readonly F: Choice4134K := Choice4134K.Has(null); "
        "end Holder4134K; "
        "class Program4134K public static method Main(): int return 16; "
        "end Main; end Program4134K;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 16);
}
