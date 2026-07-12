// Codegen — discovery sweep #6: a positional record's numeric field initialized
// from a narrower-typed literal/argument left the field's upper bits as
// uninitialized stack garbage. `record R(X: long); new R(10)` stored the i32
// literal `10` directly into the i64 field slot; with opaque pointers
// `store i32 into ptr` writes only the low 4 bytes, so `r.X` read back as
// `(garbage << 32) | 10` — `r.X = 10` was false, and a later `int(r.X + …)`
// raised a spurious OverflowException.
//
// THE FIX: buildRecordValue now coerces a positional argument to the field's
// storage LLVM type (coerceNumericToType) before the store, mirroring the class
// positional-field path. §5/§6 implicit numeric widening / §9.1.

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

// The verified repro: a single `long` field from an int literal.
BEZEL_TEST(record_widen_numeric_field, long_field_from_int_literal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record R(X: long); "
        "class C public static method Main(): int "
        "  var r := new R(10); "
        "  return (0 if r.X = 10 else 1); "  // 0
        "end Main; end C;"), 0);
}

// Two long fields summed and narrowed — previously raised a spurious
// OverflowException from the garbage upper bits.
BEZEL_TEST(record_widen_numeric_field, two_long_fields_sum_no_overflow) {
    BEZEL_EXPECT_EQ(run(
        "module M; record R(X: long, Y: long); "
        "class C public static method Main(): int "
        "  var r := new R(5, 10); "
        "  return int(r.X + r.Y) - 15; "  // 0
        "end Main; end C;"), 0);
}

// A short field (narrowing target also coerces correctly) + named-arg form.
BEZEL_TEST(record_widen_numeric_field, short_field_named_args) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(A: short, B: long); "
        "class C public static method Main(): int "
        "  var p := new P(A := 300, B := 12345); "
        "  return (0 if p.A = 300 and p.B = 12345 else 1); "  // 0
        "end Main; end C;"), 0);
}

// Control: a same-width int field is unaffected.
BEZEL_TEST(record_widen_numeric_field, int_field_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; record R(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var r := new R(7, 35); "
        "  return r.X + r.Y - 42; "  // 0
        "end Main; end C;"), 0);
}
