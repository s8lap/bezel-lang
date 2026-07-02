// Spec-derived tests for §5.1 / §19 (issue #1153): an out-of-range integer
// literal in a `case` constant or range pattern must be rejected, not silently
// truncated to the scrutinee's width.
//
// §5.1 forbids implicit narrowing of an out-of-range integer constant — the
// language rejects `var b: byte := 256` rather than truncating it. The same
// rule applies to a `case` pattern constant compared against an integer
// scrutinee: before this fix sema never range-checked the pattern literal, so
// codegen's litAtWidth() truncated it (`case x (x: byte) 256 => …` matched
// x == 0, since 256 → i8 0; a range bound `0..5000000000` over an `int`
// truncated to a 32-bit value), producing wrong matches. The fix rejects the
// out-of-range pattern literal with the §5.1 narrowing diagnostic.
//
// These are sema-level tests (resolve + typecheck): the reject is raised in the
// type checker (ExprTC::checkCasePatternConstantRanges), so an out-of-range
// pattern surfaces as a compile error and an in-range one compiles cleanly.

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

// True iff some error diagnostic mentions the §5.1 case-pattern narrowing
// reject (the message produced by checkCasePatternConstantRanges).
bool hasCasePatternRangeError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("case` pattern constant") != std::string::npos &&
            d.message.find("not implicitly convertible to the scrutinee") !=
                std::string::npos) {
            return true;
        }
    }
    return false;
}

// True iff some error diagnostic mentions the §A.7/§19.3 non-constant pattern
// reject (the message produced for a `new …` arm constant, issue #1205).
bool hasCaseNonConstantPatternError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not a valid pattern constant") != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// --- Constant pattern: the issue's exact repro (§19 / §5.1) -----------------

// `case x (x: byte) 256 => 1, _ => 0` — 256 does not fit a byte; reject it
// (previously truncated to i8 0 and wrongly matched x == 0). #1153.
BEZEL_TEST(sema_case_pattern_range, byte_const_over_range_rejected_spec_5_1) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: byte): int "
        "    return case x of 256 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasCasePatternRangeError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The byte boundary value 255 DOES fit — accepted (no error). #1153.
BEZEL_TEST(sema_case_pattern_range, byte_const_at_max_ok_spec_5_1) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: byte): int "
        "    return case x of 255 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasCasePatternRangeError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An in-range byte constant pattern is accepted and is the normal, correct
// case the fix must not disturb. #1153.
BEZEL_TEST(sema_case_pattern_range, byte_const_in_range_ok_spec_19) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: byte): int "
        "    return case x of 5 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasCasePatternRangeError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A negative constant that underflows the (unsigned) byte range is rejected. A
// signed `sbyte` scrutinee with 200 (> 127) is likewise rejected. #1153.
BEZEL_TEST(sema_case_pattern_range, sbyte_const_over_max_rejected_spec_5_1) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: sbyte): int "
        "    return case x of 200 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasCasePatternRangeError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// A comma-separated alternative arm rejects when ANY alternative is out of
// range — `1, 2, 256` over a byte still flags 256 (§10.6 / §5.1). #1153.
BEZEL_TEST(sema_case_pattern_range, multi_alternative_one_out_of_range_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: byte): int "
        "    return case x of 1 | 2 | 256 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasCasePatternRangeError(r));
}

// --- Range pattern bounds (§19 / §5.1) --------------------------------------

// A range BOUND out of the scrutinee's range is rejected: `0..5000000000` over
// an `int` (5000000000 > i32 max). Range-pattern bounds were never visited by
// sema, so they slipped past every check and truncated at codegen. #1153.
BEZEL_TEST(sema_case_pattern_range, int_range_bound_over_range_rejected_spec_5_1) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of 0..5000000000 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasCasePatternRangeError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// An in-range range pattern over an `int` is accepted (§19.1 grade idiom). The
// fix must not flag ordinary ranges. #1153.
BEZEL_TEST(sema_case_pattern_range, int_range_in_range_ok_spec_19_1) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of 0..100 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasCasePatternRangeError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Relational pattern operand (§19 / §5.1) -- issue #1156 -----------------

// A relational pattern's operand is a constant compared against the scrutinee.
// `case x (x: byte) > 300 => …` does not fit a byte; before #1156 sema never
// visited the RelOp operand, so codegen's litAtWidth() truncated 300 to i8 44
// and compiled `x > 44` — a wrong match (matched x in 45..255 instead of
// never). Reject it with the same §5.1 narrowing diagnostic. #1156.
BEZEL_TEST(sema_case_pattern_range, byte_relop_operand_over_range_rejected_spec_5_1) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: byte): int "
        "    return case x of > 300 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasCasePatternRangeError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// An in-range relational operand is accepted and matches correctly at runtime;
// the fix must not flag ordinary relational patterns (`case x > 5`). #1156.
BEZEL_TEST(sema_case_pattern_range, byte_relop_operand_in_range_ok_spec_19) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: byte): int "
        "    return case x of > 5 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasCasePatternRangeError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The other relational operators are gated identically — `<= 5000000000` over
// an `int` operand overflows i32 and is rejected (§5.1). #1156.
BEZEL_TEST(sema_case_pattern_range, int_relop_le_operand_over_range_rejected_spec_5_1) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of <= 5000000000 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasCasePatternRangeError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// --- Statement position is NOT exempt from the range gate (§19.6 / §5.1) ----

// Statement-position `case` is exempt from the exhaustiveness WARNING, but the
// literal-range gate still applies (codegen would otherwise truncate). #1153.
BEZEL_TEST(sema_case_pattern_range, statement_case_out_of_range_rejected_spec_5_1) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: byte) "
        "    case x of 256 => do return; end, _ => do return; end end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasCasePatternRangeError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// --- Non-integer scrutinees are unaffected ----------------------------------

// An enum scrutinee with bare-constant patterns must NOT trip the integer
// range gate (the gate applies only to a primitive integer scrutinee). #1153.
BEZEL_TEST(sema_case_pattern_range, enum_scrutinee_unaffected_spec_19) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public static method M(c: Color): int "
        "    return case c of Red => 1, Green => 2, Blue => 3 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasCasePatternRangeError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- §19 / §5.1 (issue #1202): a relational/range pattern operand whose -----
//     comparison class differs from the scrutinee must be rejected, not abort.
//
// Before this fix, the #964 cross-category compatibility guard was applied only
// to a ConstantPattern; a RelOpPattern operand (`> EXPR`) and a RangePattern
// lo/hi bound were unchecked. A string pointer, a bool, a float, or a decimal
// operand over an integer scrutinee (or an int operand over a string scrutinee)
// then reached codegen's CreateICmp with mismatched LLVM operand types and
// tripped `ICmpInst::AssertOK` — aborting the compiler (SIGABRT). The operand
// is now rejected at sema with the §19 compatibility diagnostic.

// True iff some error diagnostic mentions the §19 relational/range operand
// compatibility reject (the message produced by checkPatternOperand, #1202).
namespace {
bool hasCasePatternOperandError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("pattern operand type is not compatible") !=
                std::string::npos) {
            return true;
        }
    }
    return false;
}
} // namespace

// `case x > "hi"` over an `int` scrutinee — a string operand cannot be
// compared to an integer; reject (was a compiler abort). #1202.
BEZEL_TEST(sema_case_pattern_range, case_relop_type_mismatch_rejected_1202) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of > \"hi\" => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasCasePatternOperandError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// `case x > 1.5` over an `int` scrutinee — a float operand over an integer
// scrutinee (cross-numeric reconcile is a separate §19.3 ruling); reject
// rather than abort the backend. #1202.
BEZEL_TEST(sema_case_pattern_range, case_relop_float_over_int_rejected_1202) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of > 1.5 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasCasePatternOperandError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// `case x 1.5..3.5` over an `int` scrutinee — a float range bound over an
// integer scrutinee; reject (both lo and hi are incompatible). #1202.
BEZEL_TEST(sema_case_pattern_range, case_range_type_mismatch_rejected_1202) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of 1.5..3.5 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasCasePatternOperandError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// `case x > 5` over a `string` scrutinee — an integer operand over a string
// scrutinee; reject (the symmetric direction). #1202.
BEZEL_TEST(sema_case_pattern_range, case_relop_string_scrutinee_rejected_1202) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: string): int "
        "    return case x of > 5 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasCasePatternOperandError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// A COMPATIBLE relational pattern — an integer operand over an integer
// scrutinee (codegen reconciles widths) — must still be accepted. #1202.
BEZEL_TEST(sema_case_pattern_range, case_relop_compatible_ok_1202) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of > 3 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasCasePatternOperandError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A COMPATIBLE range pattern — integer bounds over an integer scrutinee —
// must still be accepted. #1202.
BEZEL_TEST(sema_case_pattern_range, case_range_compatible_ok_1202) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of 1..10 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasCasePatternOperandError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A `decimal` scrutinee with an integer relational bound must NOT be rejected:
// its codegen path (decOperand/decCmp, #1152) bridges any numeric bound, so the
// compatibility gate excludes a decimal scrutinee. Regression guard. #1202.
BEZEL_TEST(sema_case_pattern_range, case_relop_decimal_scrutinee_ok_1202) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: decimal): int "
        "    return case x of > 5 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasCasePatternOperandError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- §A.7/§19.3 non-constant expression as case pattern (#1205) --------------
//
// A `new Record(…)` expression is not a valid pattern constant under §A.7 —
// only literals, `null`, enum constants, and named `const`s are admissible.
// Before #1205 the parser wrapped the NewExpr in a ConstantPattern, sema did
// not reject it, and codegen's emitEquals / CreateICmpEQ asserted on the
// aggregate-struct operand (LLVM ICE, rc 134). The fix rejects it with a clean
// §A.7/§19.3 diagnostic before any codegen path is reached.

// §A.7/§19.3: `new Record(…)` in a case arm pattern is rejected — a
// constructor call is not a valid pattern constant. #1205.
BEZEL_TEST(sema_case_pattern_range, case_new_record_pattern_rejected_1205) {
    auto r = parseSemaAll(
        "module M; "
        "record Pt(x: int, y: int); "
        "class C "
        "  public static method M(p: Pt): int "
        "    return case p of new Pt(1, 2) => 10, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasCaseNonConstantPatternError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §A.7/§19.3: an integer literal pattern is still valid and must NOT be
// flagged by the non-constant gate introduced for #1205. #1205.
BEZEL_TEST(sema_case_pattern_range, case_literal_pattern_still_ok_1205) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(n: int): int "
        "    return case n of 1 => 10, 2 => 20, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasCaseNonConstantPatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §A.7/§19.3: an enum-constant pattern is still valid after the #1205 fix. #1205.
BEZEL_TEST(sema_case_pattern_range, case_enum_pattern_still_ok_1205) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public static method M(c: Color): int "
        "    return case c of Red => 1, Green => 2, Blue => 3 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasCaseNonConstantPatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §A.7/§19.3: `null` as a pattern constant is still valid after #1205. #1205.
BEZEL_TEST(sema_case_pattern_range, case_null_pattern_still_ok_1205) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(n: int?): int "
        "    return case n of null => 0, _ => 1 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasCaseNonConstantPatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- §19.5 / #1203: binding pattern type incompatible with a value-type scrutinee

namespace {
// True iff some error diagnostic mentions the §19.5 binding-pattern type
// incompatibility introduced for #1203.
bool hasBindingTypeIncompatError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("binding pattern type is incompatible") !=
                std::string::npos)
            return true;
    }
    return false;
}
} // namespace

// §19.5 (#1203): `var s: string` over an `int` scrutinee — a value-type
// primitive can never be a string (a reference type). Was previously compiled
// as an unconditional match (rc=1 instead of rc=2), leading to wrong-value
// results and IR_FAIL when the bound `s` was used as a string.
BEZEL_TEST(sema_case_pattern_range,
           binding_string_over_int_scrutinee_rejected_spec_19_5_1203) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of var s: string => 1, _ => 2 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasBindingTypeIncompatError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §19.5 (#1203): `var c: SomeClass` over an `int` scrutinee — a value-type
// primitive is never a class (a heap object). Was previously a SIGABRT in
// codegen (emitIsA called on an i32, not a pointer).
BEZEL_TEST(sema_case_pattern_range,
           binding_class_over_int_scrutinee_rejected_spec_19_5_1203) {
    auto r = parseSemaAll(
        "module M; "
        "class Dog end Dog; "
        "class C "
        "  public static method M(x: int): int "
        "    return case x of var d: Dog => 1, _ => 2 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasBindingTypeIncompatError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §19.5 (#1203): statement-position `case` also rejects the incompatible
// binding — the CaseStmt handler has the same check.
BEZEL_TEST(sema_case_pattern_range,
           binding_string_over_int_stmt_rejected_spec_19_5_1203) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int) "
        "    case x of var s: string => do return; end end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasBindingTypeIncompatError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §19.5 (#1203): `var n: int` over `int` scrutinee is VALID (same type) and
// must not be rejected by the new check.
BEZEL_TEST(sema_case_pattern_range,
           binding_int_over_int_scrutinee_ok_spec_19_5_1203) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of var n: int => n, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasBindingTypeIncompatError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §19.5 (#1203): `var s: string` over a `string` scrutinee is VALID (same
// type) and must not be rejected.
BEZEL_TEST(sema_case_pattern_range,
           binding_string_over_string_scrutinee_ok_spec_19_5_1203) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: string): int "
        "    return case x of var s: string => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasBindingTypeIncompatError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §19.5 (#1203): `var d: Dog` over an `Animal` scrutinee is a valid downcast
// type-test binding and must not be rejected.
BEZEL_TEST(sema_case_pattern_range,
           binding_subclass_over_base_ok_spec_19_5_1203) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal end Animal; "
        "class Dog: Animal end Dog; "
        "class C "
        "  public static method M(a: Animal): int "
        "    return case a of var d: Dog => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasBindingTypeIncompatError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §19.5 (#1203): `var n: int` over an `int?` (nullable) scrutinee is VALID —
// this is the nullable value-type unbox path (§19.5 / #784) and must not
// be rejected by the new check (the scrutinee is NULLABLE, not a plain
// value-type primitive).
BEZEL_TEST(sema_case_pattern_range,
           binding_int_over_nullable_int_ok_spec_19_5_1203) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int?): int "
        "    return case x of null => 0, var n: int => n end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasBindingTypeIncompatError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- §19 / §5.1 (issue #1213): a ConstantPattern whose constant is in a ------
//     different comparison class than the scrutinee must be rejected, not abort.
//
// Before this fix (#964) the ConstantPattern guard only covered string/bool
// class mismatches; a float or decimal constant over an integer scrutinee (or
// vice versa) still reached codegen's CreateICmp with mismatched LLVM operand
// types and tripped ICmpInst::AssertOK (SIGABRT, rc 134). The fix extends the
// guard to use the same cmpClassOf-based checkPatternOperand introduced by
// #1202 for RelOpPattern/RangePattern, so all comparison-class mismatches are
// rejected uniformly at sema (§19).

// `case x 1.5` over an `int` scrutinee — float constant over int is an ICmp
// type mismatch that aborted the compiler (rc 134); must reject cleanly. #1213.
BEZEL_TEST(sema_case_pattern_range, case_const_float_over_int_rejected_1213) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of 1.5 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasCasePatternOperandError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// `case x 1.0m` over an `int` scrutinee — decimal constant over int is also
// an ICmp type mismatch; must be rejected with the same §19 diagnostic. #1213.
BEZEL_TEST(sema_case_pattern_range, case_const_decimal_over_int_rejected_1213) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of 1.0m => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasCasePatternOperandError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// `case x 5` over an `int` scrutinee — an untyped integer literal constant
// over an integer scrutinee is COMPATIBLE and must still be accepted. #1213.
BEZEL_TEST(sema_case_pattern_range, case_const_int_over_int_ok_1213) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of 5 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasCasePatternOperandError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// `case x 2.5` over a `double` scrutinee — float constant over float scrutinee
// is COMPATIBLE (same comparison class); must be accepted. #1213.
BEZEL_TEST(sema_case_pattern_range, case_const_float_over_double_ok_1213) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: double): int "
        "    return case x of 2.5 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasCasePatternOperandError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// `case x 1.5` over a `decimal` scrutinee — decimal's codegen path bridges
// any numeric bound (decOperand/decCmp), so a float constant over a decimal
// scrutinee must NOT be rejected. Regression guard for the sc==5 exclusion.
// #1213.
BEZEL_TEST(sema_case_pattern_range, case_const_float_over_decimal_ok_1213) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: decimal): int "
        "    return case x of 1.5 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasCasePatternOperandError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
