// Regression tests for issue #8158: an INTEGER argument at a `decimal`
// parameter of a FUNCTION VALUE call (indirect closure call) must widen per
// spec §5.1 "Implicit conversions" (the `int`/`long`/`short`/`byte` → `decimal`
// rows), v0.355. Before the fix the raw integer bits reached the `%bz.decimal`
// parameter and IR verification aborted ("Call parameter type does not match
// function signature!"); the widening now goes through the shared
// `coerceNumericToType`, the ONE path every §5.1 numeric widening takes.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

void expectPrints(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// §5.1 row "`int` | `long`, `float`, `double`, `decimal`": an `int` argument
// at a `decimal` parameter of a function type.
BEZEL_TEST(funcvalue_decimal_widen_8158, int_to_decimal) {
    expectPrints(
        "module M; class C "
        "  public static method Main(): int "
        "    var g: (decimal) -> decimal := lambda(x: decimal) => x; "
        "    var i: int := 7; "
        "    Console.WriteLine(g(i)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "7\n");
}

// §5.1 row "`long` | `float`, `double`, `decimal`".
BEZEL_TEST(funcvalue_decimal_widen_8158, long_to_decimal) {
    expectPrints(
        "module M; class C "
        "  public static method Main(): int "
        "    var g: (decimal) -> decimal := lambda(x: decimal) => x; "
        "    var l: long := 9L; "
        "    Console.WriteLine(g(l)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "9\n");
}

// §5.1 row "`short` | `int`, `long`, `float`, `double`, `decimal`".
BEZEL_TEST(funcvalue_decimal_widen_8158, short_to_decimal) {
    expectPrints(
        "module M; class C "
        "  public static method Main(): int "
        "    var g: (decimal) -> decimal := lambda(x: decimal) => x; "
        "    var s: short := 11; "
        "    Console.WriteLine(g(s)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "11\n");
}

// §5.1 row "`byte` | `short`, `ushort`, `int`, `uint`, `long`, `ulong`, "
// "`float`, `double`, `decimal`".
BEZEL_TEST(funcvalue_decimal_widen_8158, byte_to_decimal) {
    expectPrints(
        "module M; class C "
        "  public static method Main(): int "
        "    var g: (decimal) -> decimal := lambda(x: decimal) => x; "
        "    var b: byte := 13; "
        "    Console.WriteLine(g(b)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "13\n");
}

// Control: a `decimal` argument at a `decimal` parameter is the identity
// conversion — it must keep working (and must not be widened twice).
BEZEL_TEST(funcvalue_decimal_widen_8158, decimal_to_decimal_control) {
    expectPrints(
        "module M; class C "
        "  public static method Main(): int "
        "    var g: (decimal) -> decimal := lambda(x: decimal) => x; "
        "    var d: decimal := 1.5m; "
        "    Console.WriteLine(g(d)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "1.5\n");
}

// The nullable lift (§5.1 "Nullable conversions": `S` converts to `T?` via
// `S → T → T?`): an `int` argument at a `decimal?` parameter of a function type.
BEZEL_TEST(funcvalue_decimal_widen_8158, int_to_nullable_decimal) {
    expectPrints(
        "module M; class C "
        "  public static method Main(): int "
        "    var g: (decimal?) -> decimal? := lambda(x: decimal?) => x; "
        "    var i: int := 7; "
        "    var r: decimal? := g(i); "
        "    Console.WriteLine(r ?? -1m); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "7\n");
}
