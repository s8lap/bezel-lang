// Regression tests for #1075 (§7.5 / §8.1 / §8.3 / §8.5): calling an inherited
// Object value-semantics method (`ToString`/`GetHashCode`/`Equals`) via `?.` on
// a NULLABLE value-type / enum / record receiver. These methods have no
// MethodSymbol, so the null-conditional lowering bailed and the `T?` result was
// zero-initialised (= null) — `n?.GetHashCode()` on a non-null `int?` yielded a
// null `int?` instead of the value; and `n?.ToString()` produced a result with a
// non-pointer IR type, so a later string compare emitted a type-mismatched ICmp
// and crashed the compiler (assertion abort). The `!.`/plain `.` forms already
// worked and are the reference behaviour.
//
// The fix factors the value-semantics dispatch core (lowerValueSemMethodOn) so
// the `?.` path lowers `hasValue ? wrap(method(inner)) : null`, and types the
// `?.`-callee value method (ToString→string?, GetHashCode→int?, Equals→bool?).
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

// #1075 wrong-value: `n?.GetHashCode()` on a NON-null `int?` returns the value
// (was a null int? → the `?? 99` then read the zero value).
BEZEL_TEST(null_cond_value_method_1075, primitive_gethashcode_present) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var n: int? := 7; "
        "  return (n?.GetHashCode()) ?? 99; "            // 7
        "end Main; end C;"), 7);
}

// The `?.` short-circuits on a NULL receiver → the `T?` result is null → `?? 99`
// returns the fallback.
BEZEL_TEST(null_cond_value_method_1075, primitive_gethashcode_null_short_circuits) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var n: int? := null; "
        "  return (n?.GetHashCode()) ?? 99; "            // 99
        "end Main; end C;"), 99);
}

// #1075 crash: `n?.ToString()` on a non-null `int?` must produce a `string?`
// (a pointer), so a downstream string compare type-checks (was: ICmp operand
// type mismatch → assertion abort, no binary).
BEZEL_TEST(null_cond_value_method_1075, primitive_tostring_string_compare) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var n: int? := 7; "
        "  using var s := n?.ToString(); "
        "  return (if s = \"7\" then 1 else 0); "                // 1
        "end Main; end C;"), 1);
}

// `n?.ToString()` on a NULL `int?` is a null `string?`.
BEZEL_TEST(null_cond_value_method_1075, primitive_tostring_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var n: int? := null; "
        "  using var s := n?.ToString(); "
        "  return (if s = null then 1 else 0); "                 // 1
        "end Main; end C;"), 1);
}

// `?.Equals(x)` on a nullable primitive → `bool?`.
BEZEL_TEST(null_cond_value_method_1075, primitive_equals_present) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var n: int? := 7; "
        "  var be := n?.Equals(7); "
        "  return (if be = true then 1 else 0); "                // 1
        "end Main; end C;"), 1);
}

// `?.GetHashCode()` on a non-null ENUM receiver (`Color?`) → the ordinal.
BEZEL_TEST(null_cond_value_method_1075, enum_gethashcode_present) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  using var g: Color? := Color.Blue; "
        "  return (g?.GetHashCode()) ?? 99; "             // Blue ordinal = 2
        "end Main; end C;"), 2);
}

// `?.ToString()` on a RECORD receiver: non-null is a live string, null is null.
BEZEL_TEST(null_cond_value_method_1075, record_tostring_present_and_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var p: Pt? := new Pt(1, 2); "
        "  var live := (if (p?.ToString()) <> null then 1 else 0); "
        "  using var pn: Pt? := null; "
        "  var none := (if (pn?.ToString()) = null then 1 else 0); "
        "  return live + none; "                          // 1 + 1 = 2
        "end Main; end C;"), 2);
}

// Regression guard: the `!.` (force-unwrap) form is unchanged.
BEZEL_TEST(null_cond_value_method_1075, force_unwrap_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var n: int? := 9; "
        "  return n!.GetHashCode(); "                     // 9
        "end Main; end C;"), 9);
}
