// Regression tests for issue #5041 (the lambda half of #4934) — §14.1 /
// §14.5 / §18.5, v0.307. The render half of #4934 is covered by
// `interp_bare_tparam_nullable_value_hole_4934_4953_test.cpp`.
//
// The repro: `Map<T>(v: T, f: (T) -> string)` at an EXPLICIT `T := int?`
// with a lambda argument. Codegen's lambda pre-pass (lowerCallDispatch)
// inferred `T` from the non-lambda argument BEFORE consulting the explicit
// type arguments, so `Map<int?>(null, …)` bound T to `null`'s reference type
// and emitted the lambda as `lam$0(ptr, ptr)` while `Util$Map$spec` passed
// the `{i8, i32}` nullable carrier — an ASan SEGV in `bz_obj_is_string`
// (plain runs only passed because the wild read did not fault).
// `Map<long?>(7L, …)` / `Map<double?>(2.5, …)` / `Map<bool?>(true, …)` hit
// the same order bug with a NON-null argument (T bound to i64 / double / i1).
// §14.5 phase 1: explicit type arguments settle the substitution; argument
// inference applies only when none are given (sema's #4935 gate).
//
// The return side: `f: (T) -> U` at an explicit `U := int?`. The lambda's
// declared return became the carrier but its sema return type stayed the
// bare `U`, so the expression body was not wrapped (`ret i32` in a
// `{i8, i32}` function — IR verifier) and a `null` body returned `ptr`.
//
// Every row asserts ran, exit 0 and the exact stdout; the ASan leg of the
// gate (BEZEL_AOT_FORCE_SANITIZE=address) is the oracle for the SEGV rows.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

void expectPrints4934Lambda(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// per §14.1 / §14.5 (v0.307), fix #4934 (lambda half) — the issue's repro:
// a generic `Map<T>(v: T, f: (T) -> string)` at `T := int?` with an
// INTERPOLATION body. The pre-#4935 ASan SEGV on `null` was hypothesised to
// come from `T` being re-bound by argument inference despite the explicit
// typeArgs; #4935's gate should make both rows produce the spec-stated output.
BEZEL_TEST(generic_method_int_nullable_closure_4934, issue_map_int_nullable_lambda_interpolation_14_1_18_5) {
    expectPrints4934Lambda(
        "module Conf; "
        "public class Util "
        "  public static method Map<T>(v: T, f: (T) -> string): string "
        "    return f(v); "
        "  end Map; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(Util.Map<int?>(null, lambda(y) => $\"<{y}>\")); "
        "    Console.WriteLine(Util.Map<int?>(5, lambda(y) => $\"<{y}>\")); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "<null>\n<5>\n");
}

// per §14.1 / §14.5 (v0.307), fix #4934 (lambda half) — same repro with a
// DIRECT method call in the lambda body (`y.ToString()`). Per L34 triage the
// `5` row already worked post-#4935; the `null` row should now produce the
// spec-stated text `null` (§18.5 "Nullable holes") because the lambda's `y`
// parameter carries the substitution-resolved `int?` (`{i8, i32}`) lowering.
BEZEL_TEST(generic_method_int_nullable_closure_4934, issue_map_int_nullable_lambda_tostring_14_1_18_5) {
    expectPrints4934Lambda(
        "module Conf; "
        "public class Util "
        "  public static method Map<T>(v: T, f: (T) -> string): string "
        "    return f(v); "
        "  end Map; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(Util.Map<int?>(5, lambda(y) => y.ToString())); "
        "    Console.WriteLine(Util.Map<int?>(null, lambda(y) => y.ToString())); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "5\nnull\n");
}

// per §14.1 (v0.307), fix #4934 (lambda half) — accept control: the bug does
// not trigger for `T := int` (the lambda's `y` is `i32`; the caller passes
// `i32`; the closure signature is `i32(i32)`). Pins the lambda parameter's
// pre-#4934 path so a future fix cannot silently regress the non-nullable
// case.
BEZEL_TEST(generic_method_int_nullable_closure_4934, accept_control_int_14_1) {
    expectPrints4934Lambda(
        "module Conf; "
        "public class Util "
        "  public static method Map<T>(v: T, f: (T) -> string): string "
        "    return f(v); "
        "  end Map; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(Util.Map<int>(5, lambda(y) => y.ToString())); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "5\n");
}

// per §14.1 (v0.307), fix #4934 (lambda half) — accept control: the bug does
// not trigger for `T := string` (a reference type; the lambda's `y` is `ptr`;
// the caller passes a `ptr`; no struct-vs-ptr mismatch).
BEZEL_TEST(generic_method_int_nullable_closure_4934, accept_control_string_14_1) {
    expectPrints4934Lambda(
        "module Conf; "
        "public class Util "
        "  public static method Map<T>(v: T, f: (T) -> string): string "
        "    return f(v); "
        "  end Map; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(Util.Map<string>(\"hi\", lambda(y) => y)); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "hi\n");
}

// per §14.5 phase 1 (v0.307), fix #5041 — sibling nullable primitives: an
// explicit `T := long?` with a `null` AND a non-null `long` argument. Before
// the fix the non-null row bound T to `i64` from the argument (lambda
// `lam(ptr, i64)` against a `{i8, i64}` specialization).
BEZEL_TEST(generic_method_int_nullable_closure_4934, issue_5041_map_long_nullable_lambda_14_5) {
    expectPrints4934Lambda(
        "module Conf; "
        "public class Util "
        "  public static method Map<T, U>(v: T, f: (T) -> U): U "
        "    return f(v); "
        "  end Map; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(Util.Map<long?, string>(null, lambda(y) => $\"<{y}>\")); "
        "    Console.WriteLine(Util.Map<long?, string>(7L, lambda(y) => $\"<{y}>\")); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "<null>\n<7>\n");
}

// per §14.5 phase 1 (v0.307), fix #5041 — sibling: explicit `T := double?`.
BEZEL_TEST(generic_method_int_nullable_closure_4934, issue_5041_map_double_nullable_lambda_14_5) {
    expectPrints4934Lambda(
        "module Conf; "
        "public class Util "
        "  public static method Map<T, U>(v: T, f: (T) -> U): U "
        "    return f(v); "
        "  end Map; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(Util.Map<double?, string>(null, lambda(y) => $\"<{y}>\")); "
        "    Console.WriteLine(Util.Map<double?, string>(2.5, lambda(y) => $\"<{y}>\")); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "<null>\n<2.5>\n");
}

// per §14.5 phase 1 (v0.307), fix #5041 — sibling: explicit `T := bool?`,
// through both the interpolation hole and a direct `y.ToString()`.
BEZEL_TEST(generic_method_int_nullable_closure_4934, issue_5041_map_bool_nullable_lambda_14_5) {
    expectPrints4934Lambda(
        "module Conf; "
        "public class Util "
        "  public static method Map<T, U>(v: T, f: (T) -> U): U "
        "    return f(v); "
        "  end Map; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(Util.Map<bool?, string>(null, lambda(y) => $\"<{y}>\")); "
        "    Console.WriteLine(Util.Map<bool?, string>(true, lambda(y) => $\"<{y}>\")); "
        "    Console.WriteLine(Util.Map<bool?, string>(false, lambda(y) => y.ToString())); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "<null>\n<True>\nFalse\n");
}

// per §14.5 / §7.2 (v0.307), fix #5041 — the RETURN side: `f: (T) -> U` at an
// explicit `U := int?`. An expression body of type `int` must be wrapped into
// the `int?` carrier, and a `null` body must produce the absent carrier.
BEZEL_TEST(generic_method_int_nullable_closure_4934, issue_5041_map_return_int_nullable_expr_body_14_5) {
    expectPrints4934Lambda(
        "module Conf; "
        "public class Util "
        "  public static method Map<T, U>(v: T, f: (T) -> U): U "
        "    return f(v); "
        "  end Map; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    var a: int? := Util.Map<int, int?>(3, lambda(y) => null); "
        "    var b: int? := Util.Map<int, int?>(3, lambda(y) => y + 1); "
        "    Console.WriteLine($\"<{a}><{b}>\"); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "<null><4>\n");
}

// per §14.5 / §7.2 (v0.307), fix #5041 — the return side with a BLOCK body
// returning both `null` and an `int`, plus an identity lambda at `T := U :=
// long?` fed `null` (parameter AND return both substituted to the carrier).
BEZEL_TEST(generic_method_int_nullable_closure_4934, issue_5041_map_return_nullable_block_body_and_identity_14_5) {
    expectPrints4934Lambda(
        "module Conf; "
        "public class Util "
        "  public static method Map<T, U>(v: T, f: (T) -> U): U "
        "    return f(v); "
        "  end Map; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    var a: int? := Util.Map<int, int?>(3, lambda(y) => do "
        "      if y > 2 then "
        "        return null; "
        "      end if; "
        "      return y; "
        "    end); "
        "    var b: int? := Util.Map<int, int?>(1, lambda(y) => do "
        "      if y > 2 then "
        "        return null; "
        "      end if; "
        "      return y; "
        "    end); "
        "    var c: long? := Util.Map<long?, long?>(null, lambda(y) => y); "
        "    var d: double? := Util.Map<double?, double?>(1.5, lambda(y) => y); "
        "    Console.WriteLine($\"<{a}><{b}><{c}><{d}>\"); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "<null><1><null><1.5>\n");
}
