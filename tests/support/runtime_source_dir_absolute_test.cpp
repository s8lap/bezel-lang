// Ratchet: the native ASan leg's runtime-source path must be ABSOLUTE, so its
// answer does not depend on the CWD of whoever runs the test binary.
//
// WHY THIS TEST EXISTS. On 2026-08-30 a machine-local ccache setting
// (`base_dir = /home/stepan` in ~/.config/ccache/ccache.conf) made a whole gate
// cycle report four unrelated suites as REAL regressions and blocked an
// 18-commit batch. ccache's base_dir rewrites every absolute path under it into
// one relative to the compiler's cwd — and it passes the REWRITTEN argv to the
// real compiler — so `-I<abs>/tests` became `-I../../../../../home/.../tests`
// and this header's __FILE__ came out relative. native_test_harness.hpp's
// runtimeSourceDir() derived src/runtime from that __FILE__, so it resolved
// from the CMake build directory and from nowhere else.
//
// The consequences were graded, and the quiet one is the worse one:
//   * runtimeSourceFiles() came back EMPTY, so sanitizerRuntimeMaster()
//     returned "" — the value ~123 test files read as "toolchain missing".
//   * A handful of those files assert on it (`BEZEL_EXPECT(rc != -1)`) and
//     FAILED loudly — measured, same binary and same tree, purely by cwd:
//     from <build>/tests the ASan rows of ownedarray_forin_using_var_1287 /
//     owning_array_drain_getowningenum_2218 / owning_enumerator_pending_value_1794
//     / channel_recv_all_1724 are 4/5/3/8 PASS (20 rows, 0 failed); from the
//     worktree root — which is where gate.sh:71 runs them — the same binary is
//     3/2/0/7, with 8 rows FAILED.
//   * The other ~117 treat "" as SKIP and PASSED VACUOUSLY. The ASan leg of
//     most of the suite was silently switched off, and nothing said so.
//
// So the mechanically-caught symptom was four false regressions, and the
// uncaught one was the loss of most of the sanitizer coverage. This test pins
// the invariant that makes both impossible: an absolute path, resolvable from
// any working directory. The fix it guards is the BEZEL_RUNTIME_SRC_DIR
// compile definition (tests/CMakeLists.txt) — a -D value is not a path
// argument, so no compiler wrapper rewrites it.
//
// If this test fails, do NOT chase the test that reported it. Check whether the
// toolchain is rewriting paths (`ccache -p | grep base_dir`) and whether
// runtimeSourceDir()'s absolute channel survived.

#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <string>

#include "native_test_harness.hpp"
#include "test_harness.hpp"

namespace {
namespace fs = std::filesystem;
}  // namespace

// The invariant itself. A relative answer here is the whole defect: every
// downstream consequence follows from it.
BEZEL_TEST(runtime_source_dir_absolute, path_is_absolute_and_is_a_directory) {
    const fs::path dir = ::bezel::native_test::runtimeSourceDir();
    BEZEL_EXPECT_TRUE(dir.is_absolute());
    std::error_code ec;
    BEZEL_EXPECT_TRUE(fs::is_directory(dir, ec));
    BEZEL_EXPECT_TRUE(!ec);
}

// Absolute is necessary but not sufficient — an absolute path at the WRONG
// place enumerates zero TUs and degrades exactly the same way. Post-#1414 the
// runtime is N per-domain TUs named bezel_runtime_*.cpp; assert we found them.
BEZEL_TEST(runtime_source_dir_absolute, enumerates_the_runtime_translation_units) {
    BEZEL_EXPECT_TRUE(::bezel::native_test::runtimeSourceFiles().size() >= 5);
}

// The property the two above only imply: the answer is the SAME from a
// different working directory. Done in a forked child so the chdir cannot
// disturb sibling tests in this process — the parent's cwd is untouched.
//
// ⚠ The chosen cwd must be DEEP. A rewritten path is `../../..<N>../<abs>`, and
// `..` at `/` is `/` — so it still resolves from ANY directory at depth <= N,
// and chdir("/") would have made this test pass over the very defect it exists
// to catch (N was 5 in the measured case). Twelve levels is comfortably past
// any real wrapper's rewrite depth: the observed ccache one was 5, and the
// deepest possible is the build directory's own depth.
BEZEL_TEST(runtime_source_dir_absolute, resolves_identically_from_a_deep_cwd) {
    const std::size_t here = ::bezel::native_test::runtimeSourceFiles().size();
    BEZEL_EXPECT_TRUE(here >= 5);

    fs::path deep = wtScratchDir("rtsrcdir");
    for (int i = 0; i < 12; ++i) deep /= "d";
    std::error_code ec;
    fs::create_directories(deep, ec);
    BEZEL_EXPECT_TRUE(!ec);
    if (ec) return;

    pid_t pid = ::fork();
    BEZEL_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        if (::chdir(deep.c_str()) != 0) ::_exit(2);
        const std::size_t there =
            ::bezel::native_test::runtimeSourceFiles().size();
        ::_exit(there == here ? 0 : 1);
    }
    int status = 0;
    BEZEL_EXPECT_TRUE(::waitpid(pid, &status, 0) == pid);
    BEZEL_EXPECT_TRUE(WIFEXITED(status));
    BEZEL_EXPECT_EQ(WEXITSTATUS(status), 0);
}
