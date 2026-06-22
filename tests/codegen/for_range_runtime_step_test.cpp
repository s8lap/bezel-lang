// Codegen — discovery sweep #8: `for var i in lo..hi step s` with a RUNTIME
// (non-constant) step crashed with "not yet implemented: codegen for this for
// statement". lowerRangeFor's inline-range path resolved the step and bailed to
// the catch-all when it was not an LLVM ConstantInt, even though §16.4 permits
// runtime-computed steps. THE FIX emits a direction-aware loop (pick the
// ascending/descending bound test by the step's runtime sign), mirroring the
// Range-variable path. §16.4.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
using bezel::test::runJit;
namespace { int run(const std::string& p){ auto r=runJit(p); return r.ran?r.exitCode:-999; } }

BEZEL_TEST(for_range_runtime_step, runtime_positive_step_ascends) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var sum: int := 0; var s: int := 2; "
        "  for var i in 0..10 step s do sum := sum + i; end; "  // 0+2+4+6+8+10
        "  return sum; end Main; end C;"), 30);
}
BEZEL_TEST(for_range_runtime_step, runtime_negative_step_descends) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var sum: int := 0; var s: int := -3; "
        "  for var i in 10..0 step s do sum := sum + i; end; "  // 10+7+4+1
        "  return sum; end Main; end C;"), 22);
}
// Control: a constant step still uses the (unchanged) compile-time-direction loop.
BEZEL_TEST(for_range_runtime_step, constant_step_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var sum: int := 0; "
        "  for var i in 0..10 step 2 do sum := sum + i; end; "  // 30
        "  return sum; end Main; end C;"), 30);
}
