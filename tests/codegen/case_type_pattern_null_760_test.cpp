// fix #760 / §19.5: a bare type pattern `T` matches only NON-NULL values.
//
// §19.5 ("Type-pattern matching never matches `null`"): "A bare type pattern T
// matches only non-null values. … `null` is matched only by the explicit `null`
// pattern, by untyped `var x` patterns …, or by `_`."
//
// The bug: for a nullable runtime-managed-reference scrutinee (`string?` — a
// pointer, not the tagged `{i8 hasValue; T value}` struct that a nullable VALUE
// type uses), `isNullableValueTy` is false, so patternMatches fell through to
// the TypePattern branch, which returned an unconditional match. A null
// `string?` therefore matched the `string` pattern (WRONG_VALUE). The fix emits
// a non-null pointer check for a nullable pointer scrutinee in that branch.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §19.5: a null `string?` must NOT match the bare `string` type pattern; the
// `_` arm fires. This is the #760 repro. → 2
BEZEL_TEST(codegen_case_type_pattern_null_760, null_string_does_not_match_type_pattern) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class P "
        "  public static method Main(): int "
        "    var x: string? := null; "
        "    var r := case x of "
        "      string => 1, "
        "      _      => 2 "
        "    end; "
        "    return r; "
        "  end Main; "
        "end P;"), 2);
}

// §19.5: a present (non-null) `string?` DOES match the bare `string` type
// pattern — the non-null requirement must not over-reject the value case. → 1
BEZEL_TEST(codegen_case_type_pattern_null_760, nonnull_string_matches_type_pattern) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class P "
        "  public static method Main(): int "
        "    var x: string? := \"hi\"; "
        "    var r := case x of "
        "      string => 1, "
        "      _      => 2 "
        "    end; "
        "    return r; "
        "  end Main; "
        "end P;"), 1);
}

// §19.5: a NON-nullable `string` is statically never null, so the bare `string`
// type pattern is an unconditional match (the non-null check is not emitted for
// a non-nullable scrutinee). → 1
BEZEL_TEST(codegen_case_type_pattern_null_760, nonnullable_string_matches_type_pattern) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class P "
        "  public static method Main(): int "
        "    var x: string := \"hi\"; "
        "    var r := case x of "
        "      string => 1, "
        "      _      => 2 "
        "    end; "
        "    return r; "
        "  end Main; "
        "end P;"), 1);
}
