// fix #778 / stdlib §4.14: the two-argument OwningBuffer<T> constructor
// `OwningBuffer<T>(length, init: (int) -> T)` calls init(i) for each i in
// 0..<length and stores the result into slot i.
//
// §4.14 (value-typed T): "The two-argument constructor calls init(i) for each
// i in 0..<length, copying the returned value into the slot."
//
// The bug: the OwningBuffer-new codegen used only the length argument
// (bz_array_alloc_count_n) and IGNORED the init lambda, so every slot stayed
// zero/null. The fix emits a 0..<length loop that calls the init closure and
// stores each result via bz_array_set.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §4.14: init `(i) => i + 1` → buf = [1, 2, 3], sum = 6. (The #778 repro.)
BEZEL_TEST(owned_buffer_init_778, two_arg_init_fills_slots) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(3, lambda(i) => i + 1); "
        "  return int(buf[0]) + int(buf[1]) + int(buf[2]); "  // 1+2+3
        "end Main; end C;"), 6);
}

// §4.14: the index is passed to init — `(i) => i * i` → [0, 1, 4], sum = 5.
BEZEL_TEST(owned_buffer_init_778, init_receives_the_index) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(3, lambda(i) => i * i); "
        "  return int(buf[0]) + int(buf[1]) + int(buf[2]); "  // 0+1+4
        "end Main; end C;"), 5);
}

// §4.14: a CAPTURING init closure works — exercises the closure env in the
// init loop. k = 10 → [10, 11, 12], sum = 33.
BEZEL_TEST(owned_buffer_init_778, capturing_init_closure) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C public static method Main(): int "
        "  var k := 10; "
        "  using var buf := new OwningBuffer<int>(3, lambda(i) => i + k); "
        "  return int(buf[0]) + int(buf[1]) + int(buf[2]); "  // 10+11+12
        "end Main; end C;"), 33);
}

// §4.14 regression guard: the single-argument constructor still zero-initializes
// every slot (the init path must not affect it). sum = 0.
BEZEL_TEST(owned_buffer_init_778, one_arg_still_zero_inits) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(3); "
        "  return int(buf[0]) + int(buf[1]) + int(buf[2]); "
        "end Main; end C;"), 0);
}
