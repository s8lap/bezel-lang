// Spec-derived sema tests for fix-forward #5173 — §19.3 / §7.11 (v0.310,
// #5143 ruling): the §7.4-narrowed-non-null-read admission for a nullable
// scrutinee at a relational/range pattern is restricted to VALUE-typed
// scrutinees.
//
// The first draft of the #5173 predicate admitted any `TypeRef::Kind::
// Primitive` as a "value-typed scrutinee", but `string` and `Object` are
// `PrimitiveType` enum values (§3 builtin types, TypeRef.hpp:30) that lower
// to heap pointers (`bz_string*`, `bz_object*`) in codegen. The matching
// codegen gate `Emitter::isNullablePrimTy` (LlvmBackend_Layout.cpp:755)
// explicitly excludes them — the only hand-rolled `#pragma` exclude list in
// the value-primitive family — so admitting them at sema routed
// patternMatches past its `isNullableVal` branch and into the
// RelOpPattern handler at LlvmBackend_Exprs2.cpp:14146, which emits
// `CreateICmpSLT(ptr, ptr)`. For two narrowed `string?` operands that is a
// SILENT WRONG VALUE: the comparison is the heap address of the `bz_string`
// structs, not the §4.6 ordinal content. Probe "z" < "a" exits 1 instead of
// 0 (reviewer's handback disassembly, .o offset 0x2c1).
//
// This file pins the predicate at the now-correct exclusion: every narrowed
// nullable REFERENCE-typed scrutinee (`string?`, `Object?`) is REJECTED at
// the §19.3 ordering-pattern gate, exactly as a class (`class K?`) and
// interface scrutinee already were (#3298's `case_relop_pattern_over_
// nullable_class_scrutinee_rejected` row, lifted_operators_rejected_3298).
//
// The narrowed VALUE-typed controls (bool?, decimal?, enum E?) stay
// ADMITTED — codegen unwraps the `{i8 hasValue; T payload}` box in
// patternMatches's `isNullableVal` branch and runs the relational/range
// comparison against `innerVal` directly, the #5173 admit path these tests
// originally opened.
//
// Out of scope: codegen path for the narrowed REFERENCE-typed form
// (`if s <> null then case s of > "b" => 1, _ => 2`) — issue #5220 (relop
// against `bz_string*` via `bz_str_compare_cp`, §4.6 ordinal). Until #5220
// lands, the narrowed reference form is the same predicate-excluded
// reference type as a class scrutinee.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
}

// §19.3 / §16.6 / §7.11 / #3298 / #5173: the predicate's diagnostic family.
// All three remedies the spec names (§7.4 narrowing, `??`, `!`) appear in
// one message; the predicate only fires from `checkOrderingPatternsOverNullable
// Scrutinee` when the scrutinee is narrowed but not a value-typed scrutinee,
// so the diagnostic text is the same one the un-narrowed reject path emits.
bool hasNullablePatternError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        const auto& m = d.message;
        if (m.find("pattern requires a non-nullable scrutinee") ==
                std::string::npos)
            continue;
        if (m.find("19.3") == std::string::npos) continue;
        if (m.find("16.6") == std::string::npos) continue;
        if (m.find("7.11") == std::string::npos) continue;
        return true;
    }
    return false;
}

}  // namespace

// fix-forward #5173 / §19.3 / §7.11: a §7.4-narrowed non-null read of a
// nullable `string?` scrutinee with a relational pattern is now REJECTED
// (the predicate now excludes `string` from the value-primitive family).
// Pre-fix-forward (predicate admitted Primitive unconditionally) this
// compiled silently and emitted `CreateICmpSLT(bz_string*, bz_string*)` at
// codegen — silent wrong code (heap address, not §4.6 ordinal). The UN-
// narrowed parameter form below (`string?` parameter, no `if x <> null`)
// was already rejected pre-fix-forward, and is still rejected (the
// narrowed-non-null-read arm is bypassed when the scrutinee Expr is a
// bare parameter).
BEZEL_TEST(scrutinee_string_narrowed_relop_5173,
           narrowed_nullable_string_scrutinee_relop_rejected_5173) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(s: string?): int "
        "  if s <> null then "
        "    var v := (case s of > \"b\" => 1, _ => 2); "
        "    return v; "
        "  end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

// Same shape, RANGE form — §19.3 / §16.6 parity.
BEZEL_TEST(scrutinee_string_narrowed_relop_5173,
           narrowed_nullable_string_scrutinee_range_rejected_5173) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(s: string?): int "
        "  if s <> null then "
        "    var v := (case s of \"a\"..\"z\" => 1, _ => 2); "
        "    return v; "
        "  end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

// Statement-position `case` — same gate at the CaseStmt visit site
// (ExprTypecheck.cpp's second call site of
// checkOrderingPatternsOverNullableScrutinee).
BEZEL_TEST(scrutinee_string_narrowed_relop_5173,
           narrowed_nullable_string_scrutinee_relop_stmt_rejected_5173) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(s: string?): int "
        "  if s <> null then "
        "    case s of "
        "      > \"b\" => return 1;, "
        "      _ => return 0; "
        "    end case; "
        "  end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

// fix-forward #5173: same predicate tightening covers `Object?`. `Object`
// is the other §3 builtin primitive (§4 class) that lowers to a heap
// pointer (`bz_object*`); codegen's `isNullablePrimTy` excludes it
// alongside `string` and `None` (the `default:` arm of the case list).
// Pre-fix-forward this also admitted silently — same `CreateICmpSLT(ptr,
// ptr)` silent-wrong-code shape as the `string?` admit.
BEZEL_TEST(scrutinee_string_narrowed_relop_5173,
           narrowed_nullable_object_scrutinee_relop_rejected_5173) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(o: Object?): int "
        "  if o <> null then "
        "    var v := (case o of > null => 1, _ => 2); "
        "    return v; "
        "  end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

// CONTROL — un-narrowed `string?` parameter: same gate (the narrowed-expr
// arm doesn't fire because `s` is a parameter with no `nullNarrowed_`
// entry). Pre- and post-fix-forward this stays REJECTED, so the row
// proves the fix didn't widen the existing un-narrowed rejection.
BEZEL_TEST(scrutinee_string_narrowed_relop_5173,
           un_narrowed_nullable_string_scrutinee_relop_rejected_5173) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(s: string?): int "
        "  var v := (case s of > \"b\" => 1, _ => 2); "
        "  return v; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

// CONTROL — narrowed VALUE-typed scrutinee (decimal?): pre- and post-fix-
// forward ADMITTED. The predicate tightening (excluding String/Object from
// the Primitive arm) is narrow — it must NOT reject the value-primitive
// admits that #5173 originally opened.
BEZEL_TEST(scrutinee_string_narrowed_relop_5173,
           narrowed_nullable_decimal_scrutinee_relop_admitted_5173) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(d: decimal?): int "
        "  if d <> null then "
        "    var v := (case d of < 2m => 1, _ => 2); "
        "    return v; "
        "  end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNullablePatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL — narrowed bool? with relational pattern. `bool` is in the
// value-primitive list (the `case P::Bool:` arm in `isNullablePrimTy`);
// the predicate must keep it admitted. (A pre-fix compiler would silently
// lower this to the existing `isNullableVal` branch, since `bool?` IS a
// nullable value type — codegen already handles it; sema admits it.)
BEZEL_TEST(scrutinee_string_narrowed_relop_5173,
           narrowed_nullable_bool_scrutinee_relop_admitted_5173) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(b: bool?): int "
        "  if b <> null then "
        "    var v := (case b of true => 1, _ => 2); "
        "    return v; "
        "  end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNullablePatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}