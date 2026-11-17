#include "test_harness.hpp"
// POSITIVE control for --selftest: two rows AFTER a helper whose parameter list has a
// default argument, in the SAME scope (as in fresh_owned_receiver_disposal_1039_test.cpp).
// The `=` inside the parameters is not an initialiser; a scanner that read it as one
// swallowed every later row of the scope and found no group here.
namespace {
int runD(const std::string& src, int fallback = -999) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : fallback;
}
BEZEL_TEST(da5, a) { BEZEL_EXPECT_EQ(runD("module M; class C public static method Main(): int return 5; end Main; end C;"), 5); }
BEZEL_TEST(da5, b) { BEZEL_EXPECT_EQ(runD("module M; class C public static method Main(): int return 5; end Main; end C;"), 5); }
// a compiled-out row: not counted, not grouped (it would otherwise twin da5.a)
#if 0
BEZEL_TEST(da5, off) { BEZEL_EXPECT_EQ(runD("module M; class C public static method Main(): int return 5; end Main; end C;"), 5); }
#endif
}  // namespace
