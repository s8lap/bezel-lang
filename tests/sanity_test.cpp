// Sanity check that the test harness itself builds, links, and runs. This is
// NOT a spec-derived test. Per the per-milestone workflow (CLAUDE.md), each
// phase's real tests are written first, derive from a cited spec section, and
// are expected to fail until the phase is implemented. The empty per-phase
// directories under tests/ are where those land.

#include "test_harness.hpp"

BEZEL_TEST(harness, runs_and_reports) {
    BEZEL_EXPECT_TRUE(true);
    BEZEL_EXPECT_EQ(1 + 1, 2);
}
