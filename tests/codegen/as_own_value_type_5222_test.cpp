// Issue #5222 (§12.7 "Value-typed operands", v0.313): `as` naming the
// operand's OWN type (or its own nullable form) is admitted and yields the
// value wrapped as `T?` -- the operand itself when it is already `T?`, null
// when that operand is null. Pre-fix, `p as int` over `p: int?` failed to
// lower (the nullable-operand arm only handled `is` / `is not`) and the local
// degraded to a zero-initialized value (#2434), so the program exited 0 where
// the spec demands the wrapped value. The printed value is the oracle.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& body) {
    return
        "module M;\n"
        "record R(X: int, Y: int);\n"
        "enum Col Red, Green, Blue; end Col;\n"
        "class C\n"
        "  public static method Main(): int\n" + body +
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

void expectOut(const std::string& body, const char* want) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

}  // namespace

// The issue's repro: exit 4 (q holds 5), not 0.
BEZEL_TEST(as_own_value_type_5222, issue_repro_nonnull_exit_4_spec_12_7_v0_313) {
    JitResult r = runAot(
        "module M; class C\n"
        "  public static method Main(): int\n"
        "    var p: int? := 5;\n"
        "    var q := p as int;\n"
        "    if q <> null then return 4; end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}

BEZEL_TEST(as_own_value_type_5222, nullable_int_as_int_spec_12_7_v0_313) {
    expectOut(
        "    var p: int? := 5;\n"
        "    var q := p as int;\n"
        "    if q <> null then Console.WriteLine(q); else Console.WriteLine(\"null\"); end;\n"
        "    var n: int? := null;\n"
        "    var m := n as int;\n"
        "    if m <> null then Console.WriteLine(m); else Console.WriteLine(\"null\"); end;\n",
        "5\nnull\n");
}

BEZEL_TEST(as_own_value_type_5222, nullable_int_as_nullable_int_spec_12_7_v0_313) {
    expectOut(
        "    var p: int? := 7;\n"
        "    var q := p as int?;\n"
        "    if q <> null then Console.WriteLine(q); else Console.WriteLine(\"null\"); end;\n"
        "    var n: int? := null;\n"
        "    var m := n as int?;\n"
        "    if m <> null then Console.WriteLine(m); else Console.WriteLine(\"null\"); end;\n",
        "7\nnull\n");
}

BEZEL_TEST(as_own_value_type_5222, nonnull_int_as_int_wraps_spec_12_7_v0_313) {
    expectOut(
        "    var i: int := 9;\n"
        "    var q := i as int;\n"
        "    if q <> null then Console.WriteLine(q); else Console.WriteLine(\"null\"); end;\n"
        "    var q2 := i as int?;\n"
        "    if q2 <> null then Console.WriteLine(q2); else Console.WriteLine(\"null\"); end;\n",
        "9\n9\n");
}

BEZEL_TEST(as_own_value_type_5222, record_as_record_spec_12_7_v0_313) {
    expectOut(
        "    var r: R := new R(3, 4);\n"
        "    var a := r as R;\n"
        "    if a <> null then Console.WriteLine(a.X + a.Y); else Console.WriteLine(\"null\"); end;\n"
        "    var rp: R? := new R(5, 6);\n"
        "    var b := rp as R;\n"
        "    if b <> null then Console.WriteLine(b.X + b.Y); else Console.WriteLine(\"null\"); end;\n"
        "    var rn: R? := null;\n"
        "    var c := rn as R;\n"
        "    if c <> null then Console.WriteLine(c.X + c.Y); else Console.WriteLine(\"null\"); end;\n",
        "7\n11\nnull\n");
}

BEZEL_TEST(as_own_value_type_5222, char_and_enum_spec_12_7_v0_313) {
    expectOut(
        "    var ch: char? := 'z';\n"
        "    var a := ch as char;\n"
        "    if a <> null then Console.WriteLine(a); else Console.WriteLine(\"null\"); end;\n"
        "    var cn: char? := null;\n"
        "    var b := cn as char;\n"
        "    if b <> null then Console.WriteLine(b); else Console.WriteLine(\"null\"); end;\n"
        "    var e: Col? := Col.Green;\n"
        "    var d := e as Col;\n"
        "    if d <> null and d = Col.Green then Console.WriteLine(\"green\"); else Console.WriteLine(\"null\"); end;\n"
        "    var en: Col? := null;\n"
        "    var f := en as Col;\n"
        "    if f <> null then Console.WriteLine(\"set\"); else Console.WriteLine(\"null\"); end;\n",
        "z\nnull\ngreen\nnull\n");
}
