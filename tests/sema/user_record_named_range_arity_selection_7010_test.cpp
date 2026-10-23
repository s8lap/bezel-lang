// fix #7010 / §2.2 (arity selection), §2.3 (always-available names),
// §16.3 (the `Range<T>` type), v0.340.
//
// THE DEFECT. `Range<T>` is an always-available builtin (§16.3 / stdlib
// §9.8) of generic arity 1, and §2.2 names exactly this collision:
//
//   "names are compared together with their generic arity ... Arity
//    selects among the types rule 1 of §2.7 finds for an identifier: a
//    reference resolves to the first, in that rule's order, whose arity
//    its type-argument list fixes"
//
// so a user `record Range(...)` (arity 0) and the builtin `Range<T>`
// (arity 1) "are two names, neither colliding with nor shadowing the
// other". `Range<int>` must therefore resolve to the BUILTIN.
//
// MEASURED on the base (5c3e10f7), the arity-selection arm in
// TypeResolution_Names.cpp cleared the user declaration and left the
// TypeRef with `declaration == nullptr` — which is NOT the shape the
// builtin's own path produces. Every identity test keyed on
// `builtinClassSym("Range")` then missed, and two §16.3 rules went
// silent:
//
//   * the element-type restriction ("`T` is restricted by the compiler to
//     the integer types ... and `char`. Other substitutions are
//     rejected — at the range expression or the `Range<T>` annotation").
//     `record Range(Lo, Hi)` + `Range<bool>` / `Range<float>` /
//     `Range<string>` all COMPILED; the identical spellings with no user
//     `Range` in scope are rejected with §16.3's diagnostic.
//   * the identity conversion (§5.1 "every type converts to itself"),
//     which compares DECLARATIONS: two null declarations compare equal,
//     so `var xs: Range<int> := r` with `r` a user `Range` record was
//     accepted. A user record is not a `Range<int>`.
//
// The fix stamps the builtin's own symbol on this arm (the same
// `builtinClassSym` / `builtinInterfaceSym` values Slice S24 assigns on
// the no-shadowing path), so both spellings now produce ONE TypeRef
// shape, and re-runs the §16.3 element check there.
//
// SCOPE. This file pins the RESOLUTION half only. The other program in
// #7010's report — `a[r]` with `r` a user record, which reaches codegen
// and fails LLVM verification with "SExt only operates on integer" — is
// NOT a `Range`-name defect at all: MEASURED, it reproduces identically
// at a user record named `Foo`, at a plain `class`, at a `string` index
// and at a `double` index. `Array<T>`'s indexer declares `[i: int]: T`
// (stdlib §4.4) and the array path never checks its index argument,
// which #6527's fix explicitly deferred ("the wider index-argument
// question — `Array<T>[j]` at a `long j` is likewise a §5.1 error ...
// is a separate gate on a different declared parameter type"). That
// gap is filed separately (#7064) and is deliberately NOT closed here.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some Error-severity diagnostic carries §16.3's element-type
// rejection — the same two needles #3247's suite uses, so this file and
// range_intrinsic_element_type_3247_test.cpp agree on what "the §16.3
// diagnostic" is.
bool hasRangeElementTypeRejected(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            (d.message.find("`Range<T>` requires T to be one of") !=
                 std::string::npos ||
             d.message.find("a `Range<T>` value requires T to be one of") !=
                 std::string::npos) &&
            d.message.find("\xC2\xA7""16.3") != std::string::npos)
            return true;
    }
    return false;
}

// True iff a §5.1 rejection is present — the identity-conversion half.
bool hasNotConvertible(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible to the target type") !=
                std::string::npos)
            return true;
    }
    return false;
}

// ============================================================================
// Part 1 — §16.3's element-type restriction must fire with a user
// `record Range` in scope. Each of these COMPILED on the base.
// ============================================================================

// §16.3 / §2.2 (v0.340): `record Range` + `Range<bool>` is a compile
// error naming §16.3. Without the user `Range` the identical spelling
// is rejected the same way (range_intrinsic_element_type_3247's
// `annotation_range_bool_rejected_spec_16_3`), so this row is the
// shadowing twin, not a new rule.
BEZEL_TEST(user_record_named_range_arity_selection_7010,
           user_range_bool_element_rejected_spec_16_3) {
    auto r = parseSemaAll(
        "module M; record Range(Lo: int, Hi: int); "
        "class C public static method Main(): int "
        "  var x: Range<bool> := 1..5; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRangeElementTypeRejected(r));
}

// §16.3 (v0.340): the same for `float` — the substitution #3247's suite
// covers without a user `Range` in scope.
BEZEL_TEST(user_record_named_range_arity_selection_7010,
           user_range_float_element_rejected_spec_16_3) {
    auto r = parseSemaAll(
        "module M; record Range(Lo: int, Hi: int); "
        "class C public static method Main(): int "
        "  var x: Range<float> := 1..5; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRangeElementTypeRejected(r));
}

// §16.3 (v0.340): ... and for `string`, which on the base reached codegen
// as a `sext` over a pointer operand.
BEZEL_TEST(user_record_named_range_arity_selection_7010,
           user_range_string_element_rejected_spec_16_3) {
    auto r = parseSemaAll(
        "module M; record Range(Lo: int, Hi: int); "
        "class C public static method Main(): int "
        "  var x: Range<string> := 1..5; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRangeElementTypeRejected(r));
}

// ============================================================================
// Part 2 — §5.1's identity conversion. A user `record Range` is not a
// `Range<int>`; the two are distinct types (§2.8.3 rule 3: a type is
// identified program-wide by its declaring module name, its qualified
// type name and its generic arity — arity 0 vs arity 1).
// ============================================================================

// §2.2 / §5.1 (v0.340): #7010's second reported program. On the base this
// compiled: both TypeRefs had `declaration == nullptr`, and the identity
// test compares declarations, so the user record passed as a `Range<int>`.
BEZEL_TEST(user_record_named_range_arity_selection_7010,
           user_record_not_assignable_to_builtin_range_spec_5_1) {
    auto r = parseSemaAll(
        "module M; record Range(Lo: int, Hi: int); "
        "class C public static method Main(): int "
        "  var r := new Range(2, 7); "
        "  var xs: Range<int> := r; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotConvertible(r));
}

// §5.1 (v0.340): the same rejection at a method ARGUMENT — the written-
// declared-type position #7010's report does not spell out, and the one
// §5.1's own "the list is illustrative, not closed" sentence covers.
BEZEL_TEST(user_record_named_range_arity_selection_7010,
           user_record_not_passable_as_builtin_range_param_spec_5_1) {
    auto r = parseSemaAll(
        "module M; record Range(Lo: int, Hi: int); "
        "class C "
        "  public static method Take(r: Range<int>): int return 0; end Take; "
        "  public static method Main(): int "
        "    var r := new Range(2, 7); "
        "    return Take(r); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotConvertible(r));
}

// §5.1 (v0.340): ... and as a FIELD's declared type. The value-builtin
// arm of checkAssignable that rejects a PRIMITIVE source reads
// `isValueBuiltinByName(to->headName())`; a non-primitive source needs the
// same verdict from the declaration comparison this fix repairs, so this
// row pins that the repair is not confined to local initializers.
BEZEL_TEST(user_record_named_range_arity_selection_7010,
           user_record_not_assignable_to_builtin_range_field_spec_5_1) {
    auto r = parseSemaAll(
        "module M; record Range(Lo: int, Hi: int); "
        "class C "
        "  public property P: Range<int>; "
        "  public static method Main(): int "
        "    var r := new Range(2, 7); "
        "    var c := new C(); "
        "    c.P := r; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotConvertible(r));
}

// ============================================================================
// Part 3 — POSITIVE CONTROLS. §2.2 says the two names neither collide
// nor shadow one another, so BOTH must keep working in the same module:
// the user's arity-0 `Range` at a bare reference, and the builtin's
// arity-1 `Range<T>` at a supplied type argument.
// ============================================================================

// §2.2 (v0.340): the user's arity-0 `Range` still wins at a BARE
// reference — the shadowing half of §2.2's "neither ... shadowing the
// other". `new Range(2, 7)` and `xs: Range` are the user's record.
BEZEL_TEST(user_record_named_range_arity_selection_7010,
           user_range_wins_at_bare_reference_spec_2_2) {
    auto r = parseSemaAll(
        "module M; record Range(Lo: int, Hi: int); "
        "class C public static method Main(): int "
        "  var r := new Range(2, 7); "
        "  var xs: Range := r; "
        "  return xs.Lo + xs.Hi; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §2.2 (v0.340): the builtin's arity-1 `Range<T>` wins at a supplied
// type argument, and it is the BUILTIN — `.Start` is stdlib §9.8's
// property, absent from the user's two-field record, so this row would
// fail if the bare reference had won.
BEZEL_TEST(user_record_named_range_arity_selection_7010,
           builtin_range_wins_at_supplied_type_argument_spec_2_2) {
    auto r = parseSemaAll(
        "module M; record Range(Lo: int, Hi: int); "
        "class C public static method Main(): int "
        "  var xs: Range<int> := 1..5; "
        "  return xs.Start.ValueOrDefault(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §2.2 (v0.340): the arity-0 vs arity-1 pair is not a collision, so a
// module may declare `Range` AND use `Range<int>` freely — including a
// `Range<int>` in the USER record's own field list, which is where both
// spellings meet in one declaration.
BEZEL_TEST(user_record_named_range_arity_selection_7010,
           user_record_may_carry_builtin_range_field_spec_2_2) {
    auto r = parseSemaAll(
        "module M; record Range(Lo: int, Hi: int); "
        "record Window(First: int, Span: Range<int>); "
        "class C public static method Main(): int "
        "  var w := new Window(1, 1..5); "
        "  return w.First; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §16.3 (v0.340): the admitted element types stay admitted with a user
// `Range` in scope — the fix must not over-fire the element check. These
// are the three #3247's suite admits (`Range<int>`, `Range<char>`,
// `Range<long>`), spelled beside the shadowing declaration.
BEZEL_TEST(user_record_named_range_arity_selection_7010,
           admitted_element_types_stay_admitted_spec_16_3) {
    for (std::string_view t : {"int", "char", "long", "byte", "uint"}) {
        auto r = parseSemaAll(
            "module M; record Range(Lo: int, Hi: int); "
            "class C public static method Main(): int "
            "  var x: Range<" + std::string(t) + "> := 1..5; "
            "  return 0; "
            "end Main; end C;");
        BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    }
}

// §2.2 / §16.3 (v0.340): `Range<T>` at a BARE generic parameter stays
// admitted — §16.3's check is on the SUBSTITUTION, and #3247's suite
// pins that carve-out. The shadowing declaration must not disturb it.
BEZEL_TEST(user_record_named_range_arity_selection_7010,
           bare_generic_param_element_stays_admitted_spec_16_3) {
    auto r = parseSemaAll(
        "module M; record Range(Lo: int, Hi: int); "
        "class C "
        "  public static method Wrap<T>(r: Range<T>): string "
        "    return r.ToString(); "
        "  end Wrap; "
        "  public static method Main(): int return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ============================================================================
// Part 4 — the SIBLING arity-selected builtins the fix stamps in the
// same arm. `Ptr<T>` (§10.5 / stdlib §5) is stamped for the same reason
// `Range` is: §16.3-style pointer equality over its synthesized symbol.
// A user `record Ptr` beside `Ptr<byte>` must behave the same way.
// ============================================================================

// §2.2 (v0.340, #7010): a user `record Ptr` does not capture the
// builtin's `Ptr<byte>` component form. The record's own field may hold a
// builtin `Ptr<byte>` (so the two spellings meet in one declaration) and
// the bare `Ptr` still names the record.
BEZEL_TEST(user_record_named_range_arity_selection_7010,
           user_record_ptr_may_carry_builtin_ptr_field_spec_2_2) {
    auto r = parseSemaAll(
        "module M; record Ptr(Addr: long); "
        "record Slot(Which: Ptr, Base: Ptr<byte>); "
        "class C public static method Main(): int "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

}  // namespace