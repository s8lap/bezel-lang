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

#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
}

// runAot drives the real AOT path (fork-isolated Driver::compileFile + clang
// link + native exec) — the strict oracle. A program that fails to compile or
// link reports `ran == false`; -999 keeps such a row from reading as an
// accidental match on a real exit code.
int runExit(const std::string& body) {
    auto r = runAot(body);
    return r.ran ? r.exitCode : -999;
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

// ===========================================================================
// #5220 — the issue's ACTUAL repro, and the coverage holes it left behind.
// ===========================================================================
//
// Every row above reaches the gate through a `string?` PARAMETER
// (`Main(s: string?)`), which this predicate rejects for a DIFFERENT reason:
// a parameter carries no `nullNarrowed_` entry, so the narrowed-non-null-read
// arm of `checkOrderingPatternsOverNullableScrutinee` never fires and the
// historical un-narrowed reject path runs instead. #5220's repro is the shape
// that DID open the admit — a §7.4-narrowed LOCAL (`var s: string? := "z";
// if s <> null then case s of < "a" => …`), where `s` is a local carrying a
// `nullNarrowed_` fact. No row above pinned that shape, so the rows guarding
// the fix-forward were guarding a sibling of the defect, not the defect.
//
// Pre-fix-forward (the predicate admitting every `TypeRef::Kind::Primitive`),
// each REJECT row below compiled cleanly and lowered to
// `CreateICmpSLT(bz_string*, bz_string*)` — comparing heap addresses, not the
// §4.6 ordinal content. Post-fix-forward each is rejected at sema with the
// §19.3 / §16.6 / §7.11 needle.

// #5220's literal repro (sema half): a §7.4-narrowed `string?` LOCAL at a
// relational pattern is REJECTED. Statement-position `case`, as filed.
BEZEL_TEST(scrutinee_string_narrowed_relop_5173,
           narrowed_local_nullable_string_scrutinee_relop_rejected_5220) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var s: string? := \"z\"; "
        "  if s <> null then "
        "    case s of < \"a\" => return 1;, _ => return 0; end case; "
        "  end if; "
        "  return 7; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

// Same narrowed LOCAL, range form — §16.6 parity with the relational row.
BEZEL_TEST(scrutinee_string_narrowed_relop_5173,
           narrowed_local_nullable_string_scrutinee_range_rejected_5220) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var s: string? := \"z\"; "
        "  if s <> null then "
        "    case s of \"a\"..\"z\" => return 1;, _ => return 0; end case; "
        "  end if; "
        "  return 7; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

// Same narrowed LOCAL for `Object?` — the other §3 builtin primitive that
// lowers to a heap pointer (`bz_object*`) and the other member of the
// value-primitive exclude list this predicate now carries.
BEZEL_TEST(scrutinee_string_narrowed_relop_5173,
           narrowed_local_nullable_object_scrutinee_relop_rejected_5220) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var o: Object? := null; "
        "  if o <> null then "
        "    case o of < null => return 1;, _ => return 0; end case; "
        "  end if; "
        "  return 7; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

// CONTROL — the narrowed LOCAL path for a VALUE-typed scrutinee, which is
// the #5173 admit the tightening must not take away. `int?` is the SAME
// `TypeRef::Kind::Primitive` arm as `string?`; only the enumerated member
// list separates them, so an over-tightened predicate would reject this.
BEZEL_TEST(scrutinee_string_narrowed_relop_5173,
           narrowed_local_nullable_int_scrutinee_relop_admitted_5220) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  if n <> null then "
        "    case n of < 7 => return 1;, _ => return 0; end case; "
        "  end if; "
        "  return 7; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNullablePatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #5220's second acceptance criterion, verbatim: "the post-fix accept with
// exit code matching ordinal semantics (e.g. 'z' < 'a' → false, 'a' < 'z' →
// true)". A `string?` carrier can only reach a relational pattern through
// the §7.8 non-null ASSERTION (`s!`), which yields a NON-NULLABLE scrutinee
// and routes to the `emitStringOrdCmp` arm the #5220 codegen work added
// (the RelOpPattern handler in LlvmBackend_Exprs2.cpp, which calls
// `bz_str_compare_cp`, §4.6). The pre-fix `CreateICmpSLT(bz_string*,
// bz_string*)` compared heap addresses instead.
//
// Both operands are string LITERALS materialized into globals, so under the
// pre-fix lowering each row answers from CG27a allocation order rather than
// content — which is exactly what makes this row separate ordinal from
// pointer order rather than merely repeat it.
BEZEL_TEST(scrutinee_string_narrowed_relop_5173,
           non_null_asserted_string_relop_ordinal_semantics_5220) {
    // "z" is GREATER than "a" (§4.6 code-point order), so `< "a"` is false
    // and the `_` arm (2) is selected.
    BEZEL_EXPECT_EQ(runExit(
        "module M; class C public static method Main(): int "
        "  var s: string? := \"z\"; "
        "  return (case s! of < \"a\" => 1, _ => 2); "
        "end Main; end C;"),
        2);
    // "a" is LESS than "z", so `< "z"` is true and arm 1 is selected.
    BEZEL_EXPECT_EQ(runExit(
        "module M; class C public static method Main(): int "
        "  var s: string? := \"a\"; "
        "  return (case s! of < \"z\" => 1, _ => 2); "
        "end Main; end C;"),
        1);
}

// The #5220 accept criterion on the VALUE-typed side: the narrowed `int?`
// local admitted above must also SELECT THE CORRECT ARM, not merely compile.
// `5 < 7` is true, so arm 1 (exit 1) — the `return 7` fall-through would mean
// the narrowed box was mis-unwrapped and the whole `case` silently skipped,
// which is the no-arm-lowered shape measured for a narrowed `record?`
// scrutinee (filed separately; not this predicate's defect).
BEZEL_TEST(scrutinee_string_narrowed_relop_5173,
           narrowed_local_nullable_int_relop_selects_arm_5220) {
    BEZEL_EXPECT_EQ(runExit(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  if n <> null then return (case n of < 7 => 1, _ => 2); end if; "
        "  return 7; "
        "end Main; end C;"),
        1);
}