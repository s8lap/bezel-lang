// Spec-derived tests for §12.9 (#4932, spec v0.307): variance applies only to
// REFERENCE-typed arguments; a value-type argument admits IDENTITY ONLY, and
// `int` / `int?` are not identical (different representation: `int?` lowers
// to a tagged `{i8, i32}` wrapper struct, `int` lowers to bare `i32`, §7.2).
// §12.9 L1592: "For value-typed T, the only legal assignment of `IEnumerable<T>`
// is to `IEnumerable<T>` itself or to `Object`." The reference-typed argument
// gate is the §12.9 reference-vs-value-type distinction at the variance
// conversion site (not the §8.6 boxing path).
//
// The unsound ACCEPT this pins: `variantGenericAssignable`'s `out`/`in` arms
// delegated per-argument admissibility to `typeArgCovariantAssignable`, whose
// early-return on `typeArgIdentical(fromArg, toArg)` short-circuited the
// value-type gate that immediately followed. `typeArgIdentical`'s base check
// (`typeArgSameBase`) intentionally ignores nullability ("variance operates
// on the base type"), so `int` was treated as identical to `int?` for the
// early-return — letting `var io: IO<int?> := ci;` where `ci: CI: IO<int>`
// slip through as a valid covariant widening. The sibling shapes (`IO<long>`
// and `IO<Object>` from `IO<int>`) were already rejected by #4892 (the
// primitive-vs-primitive value-type identity check in `typeArgCovariantAssignable`'s
// `isValueTypeArg` gate, which fires when the early-return doesn't), but the
// `int?` widening fell into the identity-is-true branch and never reached
// that gate.
//
// This fix narrows `variantGenericAssignable`'s `out`/`in` arms: when either
// side is a value type (incl. its nullable form — `isValueTypeArg` returns
// true for `int?` since `isValueTypeArg` only excludes `Object`/`String`),
// require identity (same base AND same nullability). Reference-type variance
// is untouched; identity on value types is the §12.9 reference-vs-value-type
// gate the spec mandates.

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

// True iff an ERROR diagnostic is the §5.1 non-conversion — pins THIS defect
// (the assignment was rejected as not implicitly convertible), not merely
// "some error".
bool hasConversionError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ---- FAIL-without / PASS-with: value-type variance widening is REJECTED ----

// (1) The issue's canonical repro: `CI: IO<int>` assigned to `IO<int?>`.
// Pre-fix the early-return `typeArgIdentical(int, int?) == true` fires (the
// base check ignores nullability) and the value-type gate never runs, so the
// widening is silently accepted. Post-fix the out-arm pre-check sees a value
// type on either side, distinguishes nullability, and rejects.
BEZEL_TEST(value_type_variance_identity_only_4932, int_to_nullable_int_via_covariant_iface_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "public interface IO<out T>\n"
        "  method Peek(): T;\n"
        "end IO;\n"
        "public class CI: IO<int>\n"
        "  public constructor() end constructor;\n"
        "  public method Peek(): int => 1;\n"
        "end CI;\n"
        "public class Program\n"
        "  public method M(ci: CI)\n"
        "    var io: IO<int?> := ci;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConversionError(r));
}

// (2) Mirror of (1) with `long`. A different primitive payload exercises the
// generic primitive path, not a specific-to-int specialization.
BEZEL_TEST(value_type_variance_identity_only_4932, long_to_nullable_long_via_covariant_iface_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "public interface IO<out T>\n"
        "  method Peek(): T;\n"
        "end IO;\n"
        "public class CI: IO<long>\n"
        "  public constructor() end constructor;\n"
        "  public method Peek(): long => 1;\n"
        "end CI;\n"
        "public class Program\n"
        "  public method M(ci: CI)\n"
        "    var io: IO<long?> := ci;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConversionError(r));
}

// (3) Mirror of (1) with `double`. Pins that the fix is per-value-type, not
// integer-specific.
BEZEL_TEST(value_type_variance_identity_only_4932, double_to_nullable_double_via_covariant_iface_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "public interface IO<out T>\n"
        "  method Peek(): T;\n"
        "end IO;\n"
        "public class CI: IO<double>\n"
        "  public constructor() end constructor;\n"
        "  public method Peek(): double => 1.0;\n"
        "end CI;\n"
        "public class Program\n"
        "  public method M(ci: CI)\n"
        "    var io: IO<double?> := ci;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConversionError(r));
}

// (4) Mirror of (1) with `bool`. Pins that the fix covers the bool payload,
// which #4892 already rejected as `IO<bool>` (an unrelated primitive), but
// `IO<bool?>` (nullable widening) slipped through the same way as `IO<int?>`.
BEZEL_TEST(value_type_variance_identity_only_4932, bool_to_nullable_bool_via_covariant_iface_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "public interface IO<out T>\n"
        "  method Peek(): T;\n"
        "end IO;\n"
        "public class CI: IO<bool>\n"
        "  public constructor() end constructor;\n"
        "  public method Peek(): bool => true;\n"
        "end CI;\n"
        "public class Program\n"
        "  public method M(ci: CI)\n"
        "    var io: IO<bool?> := ci;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConversionError(r));
}

// (5) Mirror of (1) with `DateTime` (a record type). `isValueTypeArg` returns
// true for record declarations, so this exercises the same out-arm pre-check
// with a non-primitive value type.
BEZEL_TEST(value_type_variance_identity_only_4932, record_datetime_to_nullable_via_covariant_iface_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "public interface IO<out T>\n"
        "  method Peek(): T;\n"
        "end IO;\n"
        "public class CI: IO<DateTime>\n"
        "  public constructor() end constructor;\n"
        "  public method Peek(): DateTime => default(DateTime);\n"
        "end CI;\n"
        "public class Program\n"
        "  public method M(ci: CI)\n"
        "    var io: IO<DateTime?> := ci;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConversionError(r));
}

// (6) Mirror of (5) with `TimeSpan` (another record type). Pins that the
// record-payload gate fires for every record, not DateTime specifically.
BEZEL_TEST(value_type_variance_identity_only_4932, record_timespan_to_nullable_via_covariant_iface_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "public interface IO<out T>\n"
        "  method Peek(): T;\n"
        "end IO;\n"
        "public class CI: IO<TimeSpan>\n"
        "  public constructor() end constructor;\n"
        "  public method Peek(): TimeSpan => default(TimeSpan);\n"
        "end CI;\n"
        "public class Program\n"
        "  public method M(ci: CI)\n"
        "    var io: IO<TimeSpan?> := ci;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConversionError(r));
}

// (7) Inverse-direction narrowing: `IO<int> := ci` where `ci: IO<int?>`. Per
// §12.9 value types admit identity only — `int?` → `int` is a NULLABILITY
// NARROWING on a value-type payload, not an identity. Rejected. Pins the
// reverse direction, ensuring the fix is not one-sided.
BEZEL_TEST(value_type_variance_identity_only_4932, nullable_int_to_int_via_covariant_iface_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "public interface IO<out T>\n"
        "  method Peek(): T;\n"
        "end IO;\n"
        "public class CI: IO<int?>\n"
        "  public constructor() end constructor;\n"
        "  public method Peek(): int? => null;\n"
        "end CI;\n"
        "public class Program\n"
        "  public method M(ci: CI)\n"
        "    var io: IO<int> := ci;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConversionError(r));
}

// ---- ACCEPT controls (regression guards) ----

// (8) IDENTITY on value types stays ACCEPTED: `IO<int?> := ci` where
// `ci: IO<int?>` is identity (same base, same nullability) — the only legal
// value-type variance assignment §12.9 permits. Confirms the strict-identity
// rule does not over-reject legitimate value-type conversions.
BEZEL_TEST(value_type_variance_identity_only_4932, nullable_int_identity_via_covariant_iface_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "public interface IO<out T>\n"
        "  method Peek(): T;\n"
        "end IO;\n"
        "public class CI: IO<int?>\n"
        "  public constructor() end constructor;\n"
        "  public method Peek(): int? => null;\n"
        "end CI;\n"
        "public class Program\n"
        "  public method M(ci: CI)\n"
        "    var io: IO<int?> := ci;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (9) Reference-type variance widening stays ACCEPTED: `Array<string>` is
// `IEnumerable<string>`'s builtin source (the §4.16 / §12.9 borrowing-collection
// covariance path). Covariance on a REFERENCE type argument (§12.9:
// "Reference-typed T admits the full covariant chain"). Confirms the fix
// does not regress §12.9's reference-type covariance rule.
BEZEL_TEST(value_type_variance_identity_only_4932, reference_string_variance_via_covariant_iface_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "public class Program\n"
        "  public method M(arr: Array<string>)\n"
        "    var xs: IEnumerable<string> := arr;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (10) Control: sibling shape `IO<long> := ci` where `ci: IO<int>` (from
// #4892) stays REJECTED. The fix must NOT relax #4892's primitive-vs-primitive
// value-type identity rule, which the out-arm pre-check also enforces via the
// `typeArgIdentical` step.
BEZEL_TEST(value_type_variance_identity_only_4932, sibling_int_to_long_via_covariant_iface_rejected_control_4892) {
    auto r = parseSemaAll(
        "module M;\n"
        "public interface IO<out T>\n"
        "  method Peek(): T;\n"
        "end IO;\n"
        "public class CI: IO<int>\n"
        "  public constructor() end constructor;\n"
        "  public method Peek(): int => 1;\n"
        "end CI;\n"
        "public class Program\n"
        "  public method M(ci: CI)\n"
        "    var io: IO<long> := ci;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConversionError(r));
}

// (11) Control: sibling shape `IO<Object> := ci` where `ci: IO<int>` (from
// #4892) stays REJECTED. The value-type-to-reference-type widening is not
// legal under §12.9 (value-typed T admits identity only or `Object` itself;
// the variance-arm pre-check covers both the primitive-int side and the
// nullability-distinguishing step).
BEZEL_TEST(value_type_variance_identity_only_4932, sibling_int_to_object_via_covariant_iface_rejected_control_4892) {
    auto r = parseSemaAll(
        "module M;\n"
        "public interface IO<out T>\n"
        "  method Peek(): T;\n"
        "end IO;\n"
        "public class CI: IO<int>\n"
        "  public constructor() end constructor;\n"
        "  public method Peek(): int => 1;\n"
        "end CI;\n"
        "public class Program\n"
        "  public method M(ci: CI)\n"
        "    var io: IO<Object> := ci;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConversionError(r));
}

// (12) Contravariant identity pin: a contravariant `in T` arm on a value-type
// argument still admits identity (and only identity). `II<int> := ci` where
// `ci: II<int>` is identity; the contravariant path in the fix's `in` arm
// must agree with the `out` arm on this case.
BEZEL_TEST(value_type_variance_identity_only_4932, contravariant_int_identity_via_in_iface_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "public interface II<in T>\n"
        "  method Put(item: T): ();\n"
        "end II;\n"
        "public class CII: II<int>\n"
        "  public constructor() end constructor;\n"
        "  public method Put(item: int): () end Put;\n"
        "end CII;\n"
        "public class Program\n"
        "  public method M(cii: CII)\n"
        "    var ii: II<int> := cii;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
