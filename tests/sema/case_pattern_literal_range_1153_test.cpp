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

} // namespace

// --- Constant pattern: the issue's exact repro (§19 / §5.1) -----------------

// `case x (x: byte) 256 => 1, _ => 0` — 256 does not fit a byte; reject it
// (previously truncated to i8 0 and wrongly matched x == 0). #1153.
BEZEL_TEST(sema_case_pattern_range, byte_const_over_range_rejected_spec_5_1) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: byte): int "
        "    return case x 256 => 1, _ => 0 end; "
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
        "    return case x 255 => 1, _ => 0 end; "
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
        "    return case x 5 => 1, _ => 0 end; "
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
        "    return case x 200 => 1, _ => 0 end; "
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
        "    return case x 1, 2, 256 => 1, _ => 0 end; "
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
        "    return case x 0..5000000000 => 1, _ => 0 end; "
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
        "    return case x 0..100 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasCasePatternRangeError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Statement position is NOT exempt from the range gate (§19.6 / §5.1) ----

// Statement-position `case` is exempt from the exhaustiveness WARNING, but the
// literal-range gate still applies (codegen would otherwise truncate). #1153.
BEZEL_TEST(sema_case_pattern_range, statement_case_out_of_range_rejected_spec_5_1) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: byte) "
        "    case x 256 => do return; end, _ => do return; end end; "
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
        "    return case c Red => 1, Green => 2, Blue => 3 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasCasePatternRangeError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
