// Ratchet: the source-scanning ratchet tests' shared repo-root discovery
// (tests/support/repo_root.hpp) must be ABSOLUTE and VALIDATED, so its answer
// does not depend on — and cannot be silently wrong because of — the CWD of
// whoever runs the test binary.
//
// WHY THIS TEST EXISTS. #3627: four ratchet files each carried an identical
// __FILE__-derived repoRoot(), the exact defect class #3468 (42cb282) fixed
// one file over in native_test_harness.hpp's runtimeSourceDir() — a ccache
// `base_dir` + `hash_dir=false` config rewrites __FILE__ to a chain of ".."
// segments that resolves only from the CMake build dir. Three of the four
// files degrade WORSE than the runtime case: a fs::recursive_directory_
// iterator over an unresolvable root silently visits ZERO files (no
// exception, no `ec` the caller reads), so a walk meant to visit ~2,000
// files visits none and a "found nothing" assertion reads as a clean tree.
// Measured: 0.045s for a nominal ~1,760-file scan is the tell.
//
// The fix (tests/support/repo_root.hpp) mirrors #3468's: an absolute -D
// (BEZEL_REPO_ROOT_DIR, tests/CMakeLists.txt) is the primary channel, with
// the __FILE__ fallback kept but now VALIDATED against landmarks every
// checkout has, so an unresolvable candidate is REJECTED (repoRoot()
// returns "") instead of silently accepted.
//
// If this test fails, do NOT chase the test that reported it. Check whether
// the toolchain is rewriting paths (`ccache -p | grep base_dir`) and whether
// repoRoot()'s absolute channel (BEZEL_REPO_ROOT_DIR) survived.

#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <string>

#include "support/repo_root.hpp"
#include "support/scratch_dir.hpp"
#include "test_harness.hpp"

namespace {
namespace fs = std::filesystem;
}  // namespace

// The invariant itself: an absolute path that actually IS this repo (carries
// the landmarks repoRoot() validates against), not merely some directory.
BEZEL_TEST(repo_root_absolute, path_is_absolute_and_is_this_repo) {
    const std::string root = ::bezel::test::repoRoot();
    BEZEL_EXPECT_TRUE(!root.empty());
    if (root.empty()) return;

    const fs::path p(root);
    BEZEL_EXPECT_TRUE(p.is_absolute());
    std::error_code ec1, ec2;
    BEZEL_EXPECT_TRUE(fs::is_regular_file(p / "CMakeLists.txt", ec1));
    BEZEL_EXPECT_TRUE(!ec1);
    BEZEL_EXPECT_TRUE(
        fs::is_regular_file(p / "docs" / "Bezel_Language_Spec.md", ec2));
    BEZEL_EXPECT_TRUE(!ec2);
}

// The property the check above only implies: the answer is the SAME from a
// different working directory. Forked so the chdir cannot disturb sibling
// tests in this process — the parent's cwd is untouched.
//
// ⚠ The chosen cwd must be DEEP, mirroring runtime_source_dir_absolute_test.
// cpp's rationale: a rewritten __FILE__ is `../../..<N>../<abs tail>`, and
// `..` at `/` is `/`, so a shallow chdir would resolve right over the very
// defect this exists to catch. Twelve levels is comfortably past any real
// wrapper's rewrite depth (the ccache incident measured N=5).
BEZEL_TEST(repo_root_absolute, resolves_identically_from_a_deep_cwd) {
    const std::string here = ::bezel::test::repoRoot();
    BEZEL_EXPECT_TRUE(!here.empty());

    fs::path deep = wtScratchDir("repo_root_deep");
    for (int i = 0; i < 12; ++i) deep /= "d";
    std::error_code ec;
    fs::create_directories(deep, ec);
    BEZEL_EXPECT_TRUE(!ec);
    if (ec) return;

    pid_t pid = ::fork();
    BEZEL_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        if (::chdir(deep.c_str()) != 0) ::_exit(2);
        const std::string there = ::bezel::test::repoRoot();
        ::_exit(there == here ? 0 : 1);
    }
    int status = 0;
    BEZEL_EXPECT_TRUE(::waitpid(pid, &status, 0) == pid);
    BEZEL_EXPECT_TRUE(WIFEXITED(status));
    BEZEL_EXPECT_EQ(WEXITSTATUS(status), 0);
}
