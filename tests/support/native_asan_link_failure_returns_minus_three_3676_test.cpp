// Ratchet: the per-file `runUnderAsan*` helpers (and the canonical
// `asan_detail::linkAndRunUnderAsan` they all delegate to or mirror) must
// report a `-fsanitize=address` link failure as `-3`, NOT as `-1`. A `-1`
// return on link failure is the #3676 conflation: the helper's caller treats
// `-1` as "toolchain unavailable: SKIP", so a real link failure (missing
// ASan runtime, bad `-l` flag, disk-full) silently reads as a SKIP and the
// row reports a clean PASS while the ASan leg is broken.
//
// #3676 fixed the named seed at `domain_spawn_choice_valuearg_3181_test.cpp`
// in commit `d9abf379` by extracting the link step into `linkRunAsan3181`
// and pinning its link-failure return to `-3`. The follow-up ratchet
// (`scripts/check_native_asan_leg_skips.py`) found 73 sibling sites in 71
// files that had the same `return -1;` shape after the link command and
// rebanked them with the same fix. This file is the family-wide regression
// test for the contract: drive the canonical helper through a forced link
// failure (bogus runtime-object path) and assert the return is `-3`. Pre-fix
// (line 104 of `tests/support/run_under_asan.hpp` returning `-1`, which the
// ratchet rebanked to `-3`), the assertion `BEZEL_EXPECT_EQ(rc, -3)` would
// have failed because the function returned `-1`; the sibling per-file
// copies that hadn't yet been migrated to `-3` would also have failed this
// test under the same setup, so the contract is pinned both at the canonical
// helper AND at the family-wide surface.
//
// The test deliberately uses the canonical helper (not one of the per-file
// copies): the canonical IS the shape every per-file copy mirrors, and the
// ratchet's role is to keep the family pinned to that shape — so a failure
// here means the ratchet has been bypassed somewhere in the per-file
// copies, and the `scripts/check_native_asan_leg_skips.py` rebank should
// have caught it; if it didn't, the bug is in the ratchet.

#include "support/run_under_asan.hpp"   // asan_detail::linkAndRunUnderAsan (#3633 canonical)
#include "native_test_harness.hpp"
#include "test_harness.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

// Quick scratch dir under the harness's existing wt-scratch path. Not the
// per-test unique_stem helper — the regression test deliberately reuses the
// same bogus paths across calls so the FAIL-WITHOUT trace is bit-identical.
fs::path regressionScratchDir() {
    const char* base = std::getenv("BEZEL_AOT_SCRATCH_DIR");
    if (!base || !*base) base = "/tmp";
    fs::path p = fs::path(base) / "bezel_3676_regression_scratch";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

}  // namespace

// fix #3676 family-wide regression: link failure under -fsanitize=address
// returns -3, NOT -1. Pre-fix the canonical's linkAndRunUnderAsan returned
// -1 on link failure (the same bug as the 73 sibling sites), which the
// caller's `if (san != -1) BEZEL_EXPECT_EQ(san, 0);` swallowed as a SKIP.
// Post-fix the helper returns -3, the caller's skip-on-(-1) does not fire,
// and the test fails loudly with `BEZEL_EXPECT_EQ(-3, 0)` instead of going
// green with a missing runtime.
//
// Drive the failure by pointing the link line at a runtime-object path that
// does not exist — the same shape runUnderAsan3181's `linkRunAsan3181` test
// uses (the spec seed at `domain_spawn_choice_valuearg_3181_test.cpp:188`).
// The link command IS the assertion: any non-zero exit means the linker
// could not produce an executable, which is exactly the failure class
// #3633 reserves `-3` for.
BEZEL_TEST(native_asan_link_failure_returns_minus_three_3676,
           link_failure_under_fsanitize_address_returns_minus_three_not_minus_one) {
    const fs::path dir = regressionScratchDir();
    const fs::path stem = dir / "bezel_3676_linkfail";
    const fs::path objPath = stem.string() + ".o";
    const fs::path exe = stem.string();

    // A non-empty .o so the link command proceeds past the input-shape check
    // and fails on the runtime-object path instead — the same shape a real
    // `clang++` install with a missing/broken ASan runtime produces (the
    // linker reports "cannot find -lasan" / "no such file" depending on
    // version, both non-zero). ELF magic, concatenated because `\x7fELF` is
    // one 5-byte escape, not `\x7f` + `"ELF"`.
    {
        std::ofstream f(objPath, std::ios::binary);
        f << "\x7f" "ELF";
    }

    const std::string bogusRtObj =
        (dir / "definitely-not-a-runtime-for-3676.o").string();

    int status = 0;
    const int rc = bezel::test::asan_detail::linkAndRunUnderAsan(
        bogusRtObj, objPath.string(), exe.string(),
        "detect_leaks=1:exitcode=99", status);

    std::remove(objPath.string().c_str());
    std::remove(exe.string().c_str());

    // The contract: link failure under -fsanitize=address is -3. Pre-fix this
    // returned -1 and a caller's `if (san != -1) BEZEL_EXPECT_EQ(san, 0);`
    // skipped the check, so a broken toolchain read as a SKIP — exactly the
    // conflation that hid #3653's detect_leaks regression until the ASan
    // sweep forced the refusal in #3682.
    BEZEL_EXPECT_EQ(rc, -3);
}