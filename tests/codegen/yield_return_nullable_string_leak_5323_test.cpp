// Regression tests for #5323 — codegen leak: `yield return` of a freshly
// minted `string?` leaks one string per yielded element; the non-nullable
// `string` twin is clean. §29 / §28.9, v0.313.
//
// THE BUG. The #2738 fix added prev-dispose at the yield store and
// Dispose-time release for non-nullable `string` elements in borrowing-head
// iterators (IEnumerable<string>). The gating flag `iterElemIsRuntimeManagedPrimitive_`
// was set only when `!elemSemaTy->nullable`, so the same fix did NOT fire
// for `string?` elements: the prev-dispose at yield store was skipped,
// and the Dispose-time release was skipped. Each `yield return <+1 producer>`
// over a `string?` element thus orphaned one refcount-1 string.
//
// THE FIX (#5323, two coordinated sites):
//   (1) LlvmBackend_Iterators.cpp emitIteratorMethod: extends the
//       runtime-managed primitive gate to also cover `string?` elements,
//       introducing a new flag `iterElemIsNullableString_` alongside
//       `iterElemIsRuntimeManagedPrimitive_`. The two are disjoint by the
//       nullable/non-nullable condition. Per §8.3/§9.1, `string?` is a
//       nullable REFERENCE type that lowers to `ptr` (not a struct), so
//       the handling mirrors the non-nullable string path exactly: load
//       the ptr, null-guard it, and if owned release it.
//   (2) LlvmBackend_Stmts.cpp yield handler: a new `iterElemIsNullableString_`
//       gate mirrors the non-nullable string prev-dispose. The owned flag
//       is set based on `isPlusOneStringProducer`, which correctly handles
//       nullable string producers (BinaryExpr `+`/`??`, ConditionalExpr,
//       numeric/Object ToString(), etc.).
//   (3) LlvmBackend_Iterators.cpp disposeYieldedVal: extends the early-return
//       gate to also fire for `iterElemIsNullableString_`, and in the
//       nullable branch emits the same null-guard + emitStringRelease
//       sequence as the yield handler.
//
// ORACLE. `runAotChecked` (compile + native exec, AddressSanitizer +
// LeakSanitizer, fork-isolated). A leak forces a nonzero exit on its own
// (`opts.sanitizeAddress = true` links the generated program with ASan+LSan).
// Each program returns a value derived from the iterated elements so the
// assertion is on BOTH correct program output AND a leak-clean exit.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Sentinel: -999 when the AOT run fails (compile failure or ASan/LSan fire
// flips `ran` to false), the program's exit code otherwise.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ============================================================================
// n1 — THE HEADLINE REPRO. §29 / §28.9: `Gen(i)` yields a freshly minted
// `string?` (the result of `Program.Fresh(i)`, which is `"f" + i.ToString()`).
// The non-nullable string twin (`IEnumerable<string>`) is clean. The loop
// iterates 200 times to make the leak measurable (one string per element).
// Pre-fix: LEAK 7290 B / 400 allocs (200 elements × {header, data}).
// Post-fix: clean.
// ============================================================================
BEZEL_TEST(yield_return_nullable_string_leak_5323,
           yield_return_fresh_string_nullable_element_for_in_loop) {
    const std::string src =
        "module M; "
        "public class Program "
        "  static method Fresh(i: int): string? => \"f\" + i.ToString(); "
        "  static method Gen(i: int): IEnumerable<string?> "
        "    yield return Program.Fresh(i); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<200 do "
        "      for var y in Program.Gen(i) do t := t + 1; end for; "
        "    end; "
        "    return t; "
        "  end Main; "
        "end Program;";
    // t = 200 (one element per call to Gen)
    BEZEL_EXPECT_EQ(run(src), 200);
}

// ============================================================================
// n2 — non-nullable string control: the same Gen(i) over `IEnumerable<string>`
// was already clean before the fix. Pins the fix did not break the non-nullable
// path. Sum of lengths: each "f" + i.ToString() has length 2 for i<10,
// 3 for i>=10. 10×2 + 10×3 = 50.
// ============================================================================
BEZEL_TEST(yield_return_nullable_string_leak_5323,
           yield_return_fresh_string_non_nullable_control) {
    const std::string src =
        "module M; "
        "public class Program "
        "  static method Fresh(i: int): string => \"f\" + i.ToString(); "
        "  static method Gen(i: int): IEnumerable<string> "
        "    yield return Program.Fresh(i); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var i in 0..<20 do "
        "      for var y in Program.Gen(i) do sum := sum + y.Length; end for; "
        "    end; "
        "    return sum; "
        "  end Main; "
        "end Program;";
    // i=0..9: "f"+i has length 2 each → 20
    // i=10..19: "f"+i has length 3 each → 30
    // Total: 50
    BEZEL_EXPECT_EQ(run(src), 50);
}

// ============================================================================
// n3 — nullable string conditional expression (if-expression) as the yield
// value. The conditional is a +1 string producer for `string?` result
// (isPlusOneStringProducer handles ConditionalExpr for nullable string).
// 3 yields per Gen call, loop 20 times = 60 total strings leaked pre-fix.
// ============================================================================
BEZEL_TEST(yield_return_nullable_string_leak_5323,
           yield_return_nullable_conditional_expr_interpolation) {
    const std::string src =
        "module M; "
        "public class Program "
        "  static method Gen(flag: bool): IEnumerable<string?> "
        "    yield return (if flag then \"yes\" else \"no\"); "
        "    yield return (if flag then $\"t-{flag}\" else $\"f-{flag}\"); "
        "    yield return \"const\"; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var count := 0; "
        "    for var i in 0..<20 do "
        "      for var y in Program.Gen(i mod 2 = 0) do count := count + 1; end for; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end Program;";
    // 3 elements per call, 20 calls = 60
    BEZEL_EXPECT_EQ(run(src), 60);
}

// ============================================================================
// n4 — nullable string literal yield: NOT a +1 producer (literal is sentinel,
// rc=∞). The owned flag stays 0; neither prev-dispose nor Dispose releases.
// This pins the fix does not gratuitously retain/release literals. Count 30
// (3 elements per Gen call × 10 iterations).
// ============================================================================
BEZEL_TEST(yield_return_nullable_string_leak_5323,
           nullable_literal_yield_not_plus_one_producer) {
    const std::string src =
        "module M; "
        "public class Program "
        "  static method Gen(): IEnumerable<string?> "
        "    yield return \"lit1\"; "
        "    yield return \"lit2\"; "
        "    yield return \"lit3\"; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var count := 0; "
        "    for var i in 0..<10 do "
        "      for var y in Program.Gen() do count := count + 1; end for; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end Program;";
    // 3 elements per call × 10 iterations = 30
    BEZEL_EXPECT_EQ(run(src), 30);
}

// ============================================================================
// n5 — nullable string null literal yield. `yield return null` is a valid
// yield value for `string?`. The nullable struct is all-zeros (null),
// the owned flag is 0. Neither prev-dispose nor Dispose does anything.
// This pins the null literal shape is handled correctly. Count 10.
// ============================================================================
BEZEL_TEST(yield_return_nullable_string_leak_5323,
           yield_return_null_literal_nullable_string) {
    const std::string src =
        "module M; "
        "public class Program "
        "  static method Gen(): IEnumerable<string?> "
        "    yield return null; "
        "    yield return \"not-null\"; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var count := 0; "
        "    for var i in 0..<10 do "
        "      for var y in Program.Gen() do count := count + 1; end for; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end Program;";
    // 2 per iteration × 10 iterations = 20
    BEZEL_EXPECT_EQ(run(src), 20);
}

// ============================================================================
// n6 — nullable string conditional expression (ToString-based) as yield value.
// `(if n <> null then n!.ToString() else "default")` is a +1 string producer
// (isPlusOneStringProducer handles ConditionalExpr for nullable string result).
// 20 iterations × 1 yield = 20 strings leaked pre-fix.
// ============================================================================
BEZEL_TEST(yield_return_nullable_string_leak_5323,
           yield_return_nullable_coalesce_expr) {
    const std::string src =
        "module M; "
        "public class Program "
        "  static method Gen(n: int?): IEnumerable<string?> "
        "    yield return (if n <> null then n!.ToString() else \"default\"); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<20 do "
        "      for var y in Program.Gen(i) do t := t + 1; end for; "
        "    end; "
        "    return t; "
        "  end Main; "
        "end Program;";
    // 1 per iteration × 20 iterations = 20
    BEZEL_EXPECT_EQ(run(src), 20);
}

// ============================================================================
// n7 — nullable string ToString() call as yield value.
// i.ToString() is a +1 producer; wrapped in nullable struct at yield.
// 20 iterations × 1 yield = 20 strings leaked pre-fix.
// ============================================================================
BEZEL_TEST(yield_return_nullable_string_leak_5323,
           yield_return_nullable_int_to_string) {
    const std::string src =
        "module M; "
        "public class Program "
        "  static method Gen(n: int): IEnumerable<string?> "
        "    yield return n.ToString(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<20 do "
        "      for var y in Program.Gen(i) do t := t + 1; end for; "
        "    end; "
        "    return t; "
        "  end Main; "
        "end Program;";
    // 1 per iteration × 20 iterations = 20
    BEZEL_EXPECT_EQ(run(src), 20);
}

// ============================================================================
// n8 — ToArray over IEnumerable<string?>. The materialize path must retain
// the curVal before appending (same as #2738 for non-nullable string).
// 50 iterations → 50 strings leaked pre-fix.
// ============================================================================
BEZEL_TEST(yield_return_nullable_string_leak_5323,
           toarray_of_nullable_fresh_strings) {
    const std::string src =
        "module M; "
        "public class Program "
        "  static method Fresh(i: int): string? => \"x\" + i.ToString(); "
        "  static method Gen(): IEnumerable<string?> "
        "    yield return Program.Fresh(1); "
        "    yield return Program.Fresh(2); "
        "    yield return Program.Fresh(3); "
        "  end Gen; "
        "  public static method Main(): int "
        "    do "
        "      using var t: Array<string?> := Program.Gen().ToArray(); "
        "      if t.Count <> 3 then return 1; end; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;";
    BEZEL_EXPECT_EQ(run(src), 0);
}
