// Spec-derived regression tests for fix #747 (R42) — two gaps in the primitive
// numeric `Parse` codegen handler.
//
// Authority: stdlib §5.2 (int.Parse / int.Parse(s, radix)) and §6.4
// (decimal.Parse). Both `int` and `decimal` reach the primitive `Parse` handler
// via their lowercase keyword receiver.
//
// Bug 1 (WRONG_VALUE): `int.Parse(s, radix)` — the two-argument form was not
// matched (the handler required exactly 1 arg), so the call was dropped and the
// result read an uninitialized slot (int.Parse("10", 16) returned 0).
//
// Bug 2 (CRASH): `decimal.Parse(s)` via the lowercase keyword reached this
// handler with pt==Decimal, matched none of the integer/float branches, and
// fell through to `return arg` — handing back the string pointer as a decimal
// struct, which crashed codegen with an IntegerType cast assertion.
//
// Fix: add a 2-arg strtol(base=radix) branch, and route the decimal case to
// bz_decimal_parse like `Decimal.Parse`.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §5.2: int.Parse(s, 16) — hexadecimal.
BEZEL_TEST(parse_overloads_747, int_parse_radix_hex) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  return int.Parse(\"ff\", 16); "  // 255
        "end Main; end F;"), 255);
}

// §5.2: int.Parse(s, 2) — binary.
BEZEL_TEST(parse_overloads_747, int_parse_radix_binary) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  return int.Parse(\"101\", 2); "  // 5
        "end Main; end F;"), 5);
}

// §5.2: the one-argument int.Parse is unaffected.
BEZEL_TEST(parse_overloads_747, int_parse_single_arg_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  return int.Parse(\"42\"); "  // 42
        "end Main; end F;"), 42);
}

// §6.4: decimal.Parse via the lowercase keyword — was a compiler crash.
BEZEL_TEST(parse_overloads_747, decimal_parse_lowercase_keyword) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  var d := decimal.Parse(\"3\"); return int(d); "  // 3
        "end Main; end F;"), 3);
}

// §6.4: the capitalized Decimal.Parse (stdlib class name) still works.
BEZEL_TEST(parse_overloads_747, decimal_parse_capitalized_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  var d := Decimal.Parse(\"7\"); return int(d); "  // 7
        "end Main; end F;"), 7);
}
