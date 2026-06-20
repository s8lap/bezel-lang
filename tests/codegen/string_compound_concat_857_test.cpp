// fix #857 / §4.6 / §25.1: a string compound concatenation `s += t` emitted
// malformed IR — the compound-assign codegen treated `+` as integer arithmetic
// regardless of operand type, feeding the string POINTERS to the integer checked-
// add intrinsic (`llvm.sadd.with.overflow` — "Intrinsic has incorrect return
// type"). The explicit `s := s + t` worked, so the defect was compound-assign-
// specific. `s += t` now mirrors `s := s + t`: emitStringConcat with the §4.6
// `string + T` RHS conversion and the `:=` retain/release discipline (transfer
// the +1 concat result into the slot, release the old value).
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// The bug #857 repro: `s += "cde"` (was a sadd-intrinsic IR verification failure).
BEZEL_TEST(string_compound_concat_857, local_string_plus_equals_string) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"ab\"; s += \"cde\"; return s.Length; "  // 5
        "end Main; end C;"), 5);
}

// Content correctness across chained `+=` (not just length).
BEZEL_TEST(string_compound_concat_857, chained_plus_equals_content) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"ab\"; s += \"cd\"; s += \"ef\"; "
        "  return (1 if s = \"abcdef\" else 0); "  // 1
        "end Main; end C;"), 1);
}

// §4.6 `string + T`: a non-string RHS (`s += 5`) converts the operand to its
// string form before concatenating.
BEZEL_TEST(string_compound_concat_857, string_plus_equals_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"n=\"; s += 5; "
        "  return (1 if s = \"n=5\" else 0); "  // 1
        "end Main; end C;"), 1);
}

// A string FIELD compound concatenation.
BEZEL_TEST(string_compound_concat_857, field_string_plus_equals) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public s: string := \"ab\"; "
        "  public constructor() end constructor; "
        "  destructor end destructor; end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  b.s += \"cd\"; "
        "  return b.s.Length; "  // 4
        "end Main; end C;"), 4);
}

// `s += t` inside a loop (the common accumulation idiom).
BEZEL_TEST(string_compound_concat_857, loop_accumulation) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"\"; "
        "  for var i in 1..3 do s += \"ab\"; end; "
        "  return s.Length; "  // 6
        "end Main; end C;"), 6);
}

// Self-aliasing `s += s` — the concat reads the slot twice; the old value is
// released exactly once after the fresh result is stored.
BEZEL_TEST(string_compound_concat_857, self_append) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"ab\"; s += s; "
        "  return (1 if s = \"abab\" else 0); "  // 1
        "end Main; end C;"), 1);
}

// Regression: a numeric `+=` is unaffected.
BEZEL_TEST(string_compound_concat_857, numeric_plus_equals_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := 5; n += 3; return n; "  // 8
        "end Main; end C;"), 8);
}
