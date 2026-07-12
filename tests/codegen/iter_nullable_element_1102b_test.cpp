// Regression tests for #1102 part B (§29.3 / §7.1): a yield-return iterator
// whose element type is a NULLABLE VALUE type (`IEnumerable<T?>`, e.g.
// `int?` / `enum?`) read every consumed element as null. The MoveNext state
// machine's current_val slot is the tagged nullable struct `{i8 hasValue, T}`,
// but a bare `yield return 5` (or `yield return null`) lowers to the payload
// type — which != the slot type — so the yield-return store at
// LlvmBackend_Stmts.cpp was SILENTLY SKIPPED (its `if (v->getType() ==
// iterElemTy_)` guard never matched), leaving current_val calloc-zeroed
// (hasValue=0 → null) for every element. The fix threads the element's sema
// type (`iterElemSemaTy_`) into the store and coerces the value with
// coerceReturnToNullableValue — a no-op for non-nullable elements, and for a
// nullable element it wraps a bare `T` into `{i8 1, T}` and maps `null` to
// `{i8 0, 0}`. Found by adversarial discovery sweep #8.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1102B: an `IEnumerable<int?>` iterator yields non-null values that the
// for-in loop variable now reads (was: always null → sum 0).
BEZEL_TEST(iter_nullable_element_1102b, all_non_null_int_q) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int?> "
        "    yield return 5; yield return 7; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var x in C.Gen() do "
        "      if x <> null then s := s + x!; end; "
        "    end; "
        "    return s; "                                  // 12
        "  end Main; end C;"), 12);
}

// A `null` yielded in the middle is read AS null (skipped), and the non-null
// values around it are summed correctly.
BEZEL_TEST(iter_nullable_element_1102b, null_in_the_middle) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int?> "
        "    yield return 5; yield return null; yield return 7; yield return null; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var nulls := 0; var sum := 0; "
        "    for var x in C.Gen() do "
        "      if x = null then nulls := nulls + 1; else sum := sum + x!; end; "
        "    end; "
        "    return nulls * 100 + sum; "                  // 2*100 + 12 = 212
        "  end Main; end C;"), 212);
}

// A nullable ENUM element type is likewise marshaled correctly.
BEZEL_TEST(iter_nullable_element_1102b, nullable_enum_element) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public static method Gen(): IEnumerable<Color?> "
        "    yield return Color.Blue; yield return null; yield return Color.Green; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var x in C.Gen() do "
        "      if x <> null then s := s + int(x!); end; "
        "    end; "
        "    return s; "                                  // 2 (Blue) + 1 (Green) = 3
        "  end Main; end C;"), 3);
}

// Regression guard: a NON-nullable `int` element iterator is unchanged
// (coerceReturnToNullableValue is a no-op for non-nullable elements).
BEZEL_TEST(iter_nullable_element_1102b, non_nullable_element_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 5; yield return 7; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var x in C.Gen() do s := s + x; end; "
        "    return s; "                                  // 12
        "  end Main; end C;"), 12);
}
