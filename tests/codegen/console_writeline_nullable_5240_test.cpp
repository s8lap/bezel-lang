// Regression test for #5240 — `Console.WriteLine(q)` with `q: T?` (a nullable
// type parameter) inside a generic body, instantiated at a value type, passes
// the nullable carrier `{i8,T}` to `bz_string_to_utf8z`, causing an IR verify
// failure.
//
// WHAT THE SPEC REQUIRES. stdlib §2.1 Console: `WriteLine(value: string)`,
// `WriteLine(value: Object)` and per-type bool/numeric overloads. "A `null`
// argument to an `Object` overload renders as the four characters `null`."
// Expected: value prints the payload; null prints `null`.
//
// ROOT CAUSE. In the specialized instantiation `G.Show<int>`, the type
// parameter `T` is substituted with `int` and `T?` with `int?`. The sema type
// of `q` (declared `var q: T?`) IS `int?` (nullable=1) in the specialized
// body, so `isNullableValueTy(argTy)` should answer true. However, `tryLowerConsoleCall`
// evaluates the argument via `lowerExpr(*call.args[0].value)` — which dispatches
// on the AST node kind of `q`. A bare `IdentifierExpr` resolves to the variable's
// `LocalDecl`, whose lowered LLVM value is the nullable struct `{i8,i32}`. The
// nullable-value branch fires, unwraps the struct, and routes correctly.
// This path works correctly when the sema type carries `nullable=1`.
//
// THE ACTUAL BUG PATH (generic body, value-type instantiation). When `q: T?`
// is declared and initialized with `var q: T? := v; Console.WriteLine(q);`
// in an UNSPECIALIZED template body, the sema type of `q` is the unresolved
// `T?` (a bare Named type parameter, nullable=1, declaration=null). In the
// specialized instantiation, the declaration's sema type IS updated to the
// concrete `int?`, BUT — the variable's lowered LLVM value is the nullable
// struct `{i8,i32}` from the specialization. The nullable-value branch fires
// when `isNullableValueTy(argTy)` is true, which is the case for the concrete
// `int?` type. So this case SHOULD work too, and indeed the probe with the
// `var q: T? := v` form now compiles and runs correctly after the #5109
// structural fix installed in this file.
//
// The #5109 fix adds STRUCTURAL detection of the nullable-value carrier as a
// fallback when `isNullableValueTy` returns false (the bare-generic-T case from
// `??` with no active substitution). It is also needed here as a belt-and-
// suspenders guard: if the sema type of `q` is not resolved as nullable at
// some point during codegen, the structural check catches the `{i8,T}` struct
// regardless.
//
// SPEC-CITED TEST CASES (per stdlib §2.1 / §5.1, v0.307):
//   T=int: value → "5", null → "null"
//   T=long: value → "42", null → "null"
//   T=bool: value → "True"/"False", null → "null"
//   T=string (reference): value → the string, null → "null" (reference null)
//   T=record (value): value → record render, null → "null"
//   T=enum (value): value → enum name, null → "null"

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

// T=int, non-null and null — the primary IR-verify failure case.
const char* kIntProgram =
    "module M; "
    "class G "
    "  public static method Show<T>(v: T?) "
    "    var q: T? := v; "
    "    Console.WriteLine(q); "
    "  end Show; "
    "end G; "
    "class P "
    "  public static method Main(): int "
    "    var p: int? := 5; "
    "    G.Show<int>(p); "
    "    return 0; "
    "  end Main; "
    "end P;";

// T=long.
const char* kLongProgram =
    "module M; "
    "class G "
    "  public static method Show<T>(v: T?) "
    "    var q: T? := v; "
    "    Console.WriteLine(q); "
    "  end Show; "
    "end G; "
    "class P "
    "  public static method Main(): int "
    "    var p: long? := 42; "
    "    G.Show<long>(p); "
    "    return 0; "
    "  end Main; "
    "end P;";

// T=bool, both true and false, and null.
const char* kBoolProgram =
    "module M; "
    "class G "
    "  public static method Show<T>(v: T?) "
    "    var q: T? := v; "
    "    Console.WriteLine(q); "
    "  end Show; "
    "end G; "
    "class P "
    "  public static method Main(): int "
    "    G.Show<bool>(true); "
    "    G.Show<bool>(false); "
    "    var n: bool? := null; "
    "    G.Show<bool>(n); "
    "    return 0; "
    "  end Main; "
    "end P;";

// NOTE: T=string (nullable reference), T=record, and T=enum cases are
// covered by separate regression files for those specific paths. This file
// focuses on the #5240 IR-verify failure (value-type instantiation).

// Control: non-generic int? (the pre-existing, always-correct behaviour).
const char* kNonGenericIntProgram =
    "module M; "
    "class P "
    "  public static method Main(): int "
    "    var q: int? := 5; "
    "    Console.WriteLine(q); "
    "    var n: int? := null; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end P;";

}  // namespace

// §5.1 / #5240: T=int, value-type instantiation, primary IR-verify case.
BEZEL_TEST(console_writeline_nullable_5240, int_value_type_instantiation) {
    BEZEL_EXPECT_EQ(runAotOut(kIntProgram), "5");
}

// §5.1 / #5240: T=long.
BEZEL_TEST(console_writeline_nullable_5240, long_value_type_instantiation) {
    BEZEL_EXPECT_EQ(runAotOut(kLongProgram), "42");
}

// §5.1 / #5240: T=bool, true / false / null.
BEZEL_TEST(console_writeline_nullable_5240, bool_value_type_instantiation) {
    BEZEL_EXPECT_EQ(runAotOut(kBoolProgram), "True\nFalse\nnull");
}

// #5240 control: non-generic int? — pre-existing behaviour must stay exact.
BEZEL_TEST(console_writeline_nullable_5240, non_generic_int_control) {
    BEZEL_EXPECT_EQ(runAotOut(kNonGenericIntProgram), "5\nnull");
}
