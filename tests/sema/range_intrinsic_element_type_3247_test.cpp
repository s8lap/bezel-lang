// fix #3247 / §16.3 (v0.277): `Range<T>`'s T is restricted by the compiler
// to the integer types (byte, sbyte, short, ushort, int, uint, long, ulong)
// and char. "Other substitutions are rejected. This restriction is enforced
// as a compiler-intrinsic rule." Before this fix the rejection was IMPLICIT
// at codegen — the `sext <ty> to i64` for a float/double/decimal/string
// element typed an `i64 sext` over a non-integer operand and the LLVM
// verifier rejected it ("SExt only operates on integer"); the user saw a
// malformed-IR / ICE-class diagnostic, not a clean §16.3 rejection. The
// new front-end gate fires at the type-annotation site (so the diagnostic
// points at the type) AND at the range-expression site (so an inferred
// `1.0..5.0` — no annotation — is also caught with a clean message).
//
// This file is the regression suite. Positive controls (Range<int>,
// Range<char>, Range<long>) confirm the gate does NOT over-fire. Negative
// cases (Range<float>, Range<double>, Range<decimal>, Range<string>,
// Range<bool>) confirm the §16.3 diagnostic is loud at the right site.
//
// The existing case-pattern fix (tests/sema/case_pattern_range_divergence_
// 3319_test.cpp) covers range PATTERNS over non-integer scrutinees; the
// case there is the *pattern* `1.0..5.0` matched against an `x: double`
// scrutinee, not the *type* `Range<double>`. Both halves of §16.3
// (pattern-side and type-side) need the same loud-reject discipline.

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

// True iff some Error-severity diagnostic carries the §16.3 rejection text
// the fix installed. Two needles: the lead phrase (one of the two sites
// — annotation or range expression) and the §16.3 citation.
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

// True iff a §16.3 rejection diagnostic names the given element type
// (e.g. "got `float`", "got `double`", "got `decimal`").
bool hasRangeElementTypeRejectedName(const bezel::test::SemaResult& r,
                                     std::string_view typeName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("got `") != std::string::npos &&
            d.message.find(std::string(typeName)) != std::string::npos &&
            d.message.find("\xC2\xA7""16.3") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// ============================================================================
// Part 1 — ANNOTATION form (`var r: Range<NonInteger> := ...`).
// The diagnostic points at the type annotation; the fix fires the §16.3
// gate at TypeResolution_Names.cpp's Range<T> case, so the user sees the
// type, not the downstream range expression.
// ============================================================================

// §16.3 / #3247: `var r: Range<float>` is a compile error.
BEZEL_TEST(range_intrinsic_element_type_3247,
           annotation_range_float_rejected_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var r: Range<float> := 1.0..5.0; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRangeElementTypeRejected(r));
    BEZEL_EXPECT_TRUE(hasRangeElementTypeRejectedName(r, "float"));
}

// §16.3 / #3247: `var r: Range<double>` is a compile error.
BEZEL_TEST(range_intrinsic_element_type_3247,
           annotation_range_double_rejected_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var r: Range<double> := 1.0..5.0; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRangeElementTypeRejected(r));
    BEZEL_EXPECT_TRUE(hasRangeElementTypeRejectedName(r, "double"));
}

// §16.3 / #3247: `var r: Range<decimal>` is a compile error.
BEZEL_TEST(range_intrinsic_element_type_3247,
           annotation_range_decimal_rejected_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var r: Range<decimal> := 1m..5m; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRangeElementTypeRejected(r));
    BEZEL_EXPECT_TRUE(hasRangeElementTypeRejectedName(r, "decimal"));
}

// §16.3 / #3247: `var r: Range<string>` is a compile error (the prior
// code reached codegen and produced "SExt only operates on integer" over
// a string pointer sext).
BEZEL_TEST(range_intrinsic_element_type_3247,
           annotation_range_string_rejected_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var r: Range<string> := \"a\"..\"z\"; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRangeElementTypeRejected(r));
    BEZEL_EXPECT_TRUE(hasRangeElementTypeRejectedName(r, "string"));
}

// §16.3 / #3247: `var r: Range<bool>` is a compile error. The prior code
// rejected `Range<bool>` at sema for a different reason (bool is not
// implicitly convertible to any integer type), so the §16.3 diagnostic
// is the user-visible reason the program is rejected.
BEZEL_TEST(range_intrinsic_element_type_3247,
           annotation_range_bool_rejected_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var r: Range<bool> := false..true; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRangeElementTypeRejected(r));
}

// ============================================================================
// Part 2 — INFERRED form (`var r := 1.0..5.0;`). The annotation gate
// cannot see this — the type is inferred from the range expression's
// endpoints, so the §16.3 gate must fire in ExprTC::visitExpr for the
// RangeExpr case. Without the fix, `1.0..5.0` reaches codegen and
// produces "SExt only operates on integer" over a float sext.
// ============================================================================

// §16.3 / #3247: `var r := 1.0..5.0;` (inferred Range<double>) is a
// compile error at the range expression, with a clean §16.3 message.
BEZEL_TEST(range_intrinsic_element_type_3247,
           inferred_range_double_rejected_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var r := 1.0..5.0; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRangeElementTypeRejected(r));
    BEZEL_EXPECT_TRUE(hasRangeElementTypeRejectedName(r, "double"));
}

// §16.3 / #3247: `var r := 1.0..2.0 step 0.5;` is a compile error.
BEZEL_TEST(range_intrinsic_element_type_3247,
           inferred_range_float_with_step_rejected_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var r := 1.0..2.0 step 0.5; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRangeElementTypeRejected(r));
    BEZEL_EXPECT_TRUE(hasRangeElementTypeRejectedName(r, "double"));
}

// §16.3 / #3247: a half-open inferred `var r := 1m..<5m;` is also
// rejected — the §16.3 gate is independent of `..` vs `..<`.
BEZEL_TEST(range_intrinsic_element_type_3247,
           inferred_range_decimal_half_open_rejected_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var r := 1m..<5m; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRangeElementTypeRejected(r));
    BEZEL_EXPECT_TRUE(hasRangeElementTypeRejectedName(r, "decimal"));
}

// ============================================================================
// Part 3 — POSITIVE CONTROLS. The §16.3 gate must NOT over-fire; the
// integer family and char are the cases the spec admits. The pre-fix
// behavior already admitted these, so the new gate's admit path is the
// regression floor.
// ============================================================================

// §16.3 / #3247 (positive control): `var r: Range<int>` is admitted —
// the §16.3 explicit-allowed set. Sum 1+2+3+4+5 = 15, exit 15.
BEZEL_TEST(range_intrinsic_element_type_3247,
           annotation_range_int_ok_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 1..5; "
        "  var n: int := 0; "
        "  for var i in r do n := n + i; end; "
        "  return n; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §16.3 / #3247 (positive control): the inferred `var r := 1..5` is
// admitted (the range expression is the RangeExpr's inferred form).
// Sum 1+2+3+4+5 = 15, exit 15.
BEZEL_TEST(range_intrinsic_element_type_3247,
           inferred_range_int_ok_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var r := 1..5; "
        "  var n: int := 0; "
        "  for var i in r do n := n + i; end; "
        "  return n; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §16.3 / #3247 (positive control): `var r: Range<long>` is admitted
// (the §16.3 explicit-allowed set includes long).
BEZEL_TEST(range_intrinsic_element_type_3247,
           annotation_range_long_ok_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var r: Range<long> := 1L..5L; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §16.3 / #3247 (positive control): `var r: Range<char>` is admitted
// (§16.3's char inclusion — the surrogate-gap-stepping path is keyed
// off char specifically, LlvmBackend_Stmts.cpp:5063).
BEZEL_TEST(range_intrinsic_element_type_3247,
           annotation_range_char_ok_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var r: Range<char> := 'a'..'z'; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §16.3 / #3247 (positive control): all eight integer primitives are
// admitted — the spec's admit set is byte/sbyte/short/ushort/int/uint/
// long/ulong. Picking two less-common ones (byte and ulong) guards
// against an over-narrow gate that admits only `int`/`long`.
//
// Byte/ulong have no literal suffix (§4.2), so the endpoints are cast
// explicitly to the matching primitive. The cast form `byte(N)` is the
// spec-stated way to form a byte-typed value at a site where a literal
// would otherwise default to `int`; the §16.3 gate reads the resulting
// RangeExpr's element type and admits it, which is the admit path we
// want to exercise here. (The existing `tests/codegen/array_sized_fill_
// 1375_stdlib_4_4_test.cpp` uses the same `byte(N)` form for the same
// reason.)
BEZEL_TEST(range_intrinsic_element_type_3247,
           annotation_range_byte_and_ulong_ok_spec_16_3) {
    auto r1 = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var r: Range<byte> := byte(0)..byte(100); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r1.diagnostics.hasErrors());
    auto r2 = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var r: Range<ulong> := 0UL..1000UL; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r2.diagnostics.hasErrors());
}

// fix #3247 (narrow-fix-forward for #3242 / v0.277): an UNRESOLVED generic
// type-parameter T — a `Range<T>` reference inside a generic body, before
// the §29.3 substitution lands at the instantiation site — is ADMITTED at
// the type-annotation site. The §16.3 restriction applies to the SUBSTITUTED
// T (T=int, T=char, T=float, ...), not to the bare T: rejecting the bare T
// at the declaration would bar every `public method Wrap<T>(r: Range<T>)`
// whose T will later be bound to char / int (the supported substitutions),
// even though the #3242 codegen tests cover exactly that generic-substitution
// shape. Detection at this site is "kind == Named and declaration == nullptr"
// — resolveNamed (TypeResolution_Names.cpp:1548-1554) records an unresolved
// generic param as a Named TypeRef with no declaration because type-param
// substitution is the SEMANTIC pass's job, not the resolver's. The downstream
// instantiation site re-runs the §16.3 predicate on the substituted T and
// rejects there if the bound violates §16.3 (the float/double generic
// negative cases below).
//
// Pre-fix: the gate over-fired on bare T and rejected every Range<T> in a
// generic body. Post-fix: the bare T is admitted; substituted T continues
// to gate as before.

// §16.3 / #3247 (carve-out, positive): `Range<T>` in a generic body, before
// the substitution lands, is admitted. The annotation `r: Range<T>` must
// pass without diagnostics; the runtime bound at the instantiation site is
// not this layer's concern.
BEZEL_TEST(range_intrinsic_element_type_3247,
           annotation_range_generic_param_admitted_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Wrap<T>(r: Range<T>): string "
        "    return r.ToString(); "
        "  end Wrap; "
        "  public static method Main(): int return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §16.3 / #3247 (carve-out, negative guard): the carve-out is for the
// UNRESOLVED generic type parameter — a concrete substituted type that
// violates §16.3 (here: `Range<float>` passed as the generic substitution)
// must STILL be rejected at the type-annotation site. The substitution
// resolution at the call site is the layer where the substituted T's
// type (double) becomes visible; the §16.3 gate fires then, not at the
// generic declaration. A naive carve-out that also admitted concrete
// non-integer types would lose this guarantee.
BEZEL_TEST(range_intrinsic_element_type_3247,
           annotation_range_float_via_substitution_still_rejected_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main(): int "
        "    var s := Wrap(1.5..3.5); "
        "  end Main; "
        "  public static method Wrap<T>(r: Range<T>): string "
        "    return r.ToString(); "
        "  end Wrap; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRangeElementTypeRejected(r));
}
