// fix #852 / §25.1: a user-defined unary `operator +` on a record (or class) was
// silently ignored — `+x` lowered to the primitive identity instead of calling
// the overload. sema's findUnaryOperator only resolved `-` and `~`, omitting `+`
// (which §25.1 lists as overloadable). With `+` added, `+x` dispatches to the
// record's `operator +(c)`; the `params.size() == 1` filter keeps the unary form
// distinct from a binary `operator +(a, b)`.
//
// Value tests — runJit returns the raw int (kept in 0..255).

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

// Unary `+` overload on a record is called (was identity). The bug09 repro.
BEZEL_TEST(unary_plus_overload_852, record_unary_plus_calls_overload) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "record Counter(Value: int)\n"
        "  public static method operator +(c: Counter): Counter => new Counter(99);\n"
        "end Counter;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var c := new Counter(5);\n"
        "    var d := +c;\n"
        "    return d.Value;\n"          // 99, not 5
        "  end Main;\n"
        "end C;\n"), 99);
}

// A record declaring BOTH unary `+(c)` and binary `+(a, b)`: `+x` must select the
// 1-param unary overload, `x + y` the 2-param binary one.
BEZEL_TEST(unary_plus_overload_852, unary_and_binary_plus_disambiguated) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "record N(V: int)\n"
        "  public static method operator +(a: N): N => new N(a.V + 1);\n"
        "  public static method operator +(a: N, b: N): N => new N(a.V + b.V);\n"
        "end N;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var x := new N(10);\n"
        "    var y := new N(20);\n"
        "    var u := +x;\n"             // unary → 11
        "    var s := x + y;\n"          // binary → 30
        "    return u.V + s.V;\n"        // 41
        "  end Main;\n"
        "end C;\n"), 41);
}

// Regression: unary `-` overload still resolves (the path `+` was added alongside).
BEZEL_TEST(unary_plus_overload_852, unary_minus_overload_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "record Counter(Value: int)\n"
        "  public static method operator -(c: Counter): Counter => new Counter(0 - c.Value);\n"
        "end Counter;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var c := new Counter(7);\n"
        "    var d := -c;\n"
        "    return 0 - d.Value;\n"      // 7
        "  end Main;\n"
        "end C;\n"), 7);
}

// Regression: primitive unary `+` stays the identity (no overload exists, so
// findUnaryOperator returns null and codegen falls through to `return v`).
BEZEL_TEST(unary_plus_overload_852, primitive_unary_plus_identity) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int\n"
        "  var x := 42; return +x;\n"
        "end Main; end C;\n"), 42);
}
