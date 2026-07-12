// Regression pin for the AOT harness's forked-compile CRASH ISOLATION (qr1 M1;
// aot_support.hpp forkedCompile). This is the "regression-test-pins-the-bug"
// gtest for that property: it FAILS loudly if the fork isolation is ever removed.
//
// The property. runAot compiles the source via Driver::compileFile run in a
// fork()ed CHILD. If the compiler CRASHES on some input — the front-end has a
// live crash history on error-recovery ASTs (#1622/#1615), and negative tests
// deliberately feed it malformed programs — the crash must die in the child and
// surface as an ISOLATED ran=false; the parent test process must SURVIVE so the
// rest of the shard still runs. Without the fork, the SIGSEGV would abort the
// whole bezel_tests binary, taking down every other test in the shard.
//
// How this pins it. The test FORCES a deterministic compiler crash via the
// BEZEL_AOT_TEST_CRASH_HOOK affordance (aot_support.hpp: the forked compile child
// raise(SIGSEGV)s before doing any work). Then it asserts runAot returned an
// isolated ran=false — which it can only reach if the PARENT survived the crash.
// If a future change moves the compile back in-process (or otherwise removes the
// fork), this same call crashes the parent and the binary aborts HERE with
// SIGSEGV: a loud, unmissable regression.
//
// Fail-without / pass-with (verified qr2): with the fork in place this test
// passes (ran=false, parent alive); temporarily moving the raise() to BEFORE the
// fork() in forkedCompile (i.e. disabling isolation) makes this test abort the
// entire bezel_tests binary with SIGSEGV instead of passing.

#include <cstdlib>
#include <string_view>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// A well-formed program. It is never actually compiled when the crash hook is
// armed: the child raises SIGSEGV before compileFile runs. Using a VALID program
// makes clear the ran=false is due to the injected COMPILER CRASH, not a
// legitimate program rejection.
constexpr const char* kValidProgram =
    "module M; class C public static method Main(): int "
    "  return 0; end Main; end C;";

bool diagContains(const JitResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string_view::npos) return true;
    return false;
}

}  // namespace

// The load-bearing test: a compiler crash in the forked compile child is
// isolated to ran=false and the parent test process survives it.
BEZEL_TEST(aot_crash_isolation, compiler_crash_in_child_is_isolated) {
    // Arrange: force the forked compile child to SIGSEGV before it compiles.
    ::setenv("BEZEL_AOT_TEST_CRASH_HOOK", "1", /*overwrite=*/1);
    JitResult r = runAot(kValidProgram);
    ::unsetenv("BEZEL_AOT_TEST_CRASH_HOOK");

    // Reaching this line AT ALL is the core assertion: the parent survived a
    // compiler crash (proof of fork isolation, qr1 M1). Had the crash not been
    // isolated, the binary would have died before here.
    BEZEL_EXPECT_TRUE(!r.ran);                     // isolated -> program not run
    BEZEL_EXPECT_EQ(r.exitCode, 0);                // never ran; exitCode untouched
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());  // carries a diagnostic
    // The crash path labels it a COMPILER CRASH (distinct from a program reject)
    // so triage sees a compiler bug to file, not an ordinary rejection.
    BEZEL_EXPECT_TRUE(diagContains(r, "compiler CRASHED"));
}

// The isolated crash must leave NO wedged state in the parent (leftover child,
// redirected fds, etc.): a subsequent normal compile+run works. Every other AOT
// test in the suite depends on the same toolchain, so we assert firmly.
BEZEL_TEST(aot_crash_isolation, harness_recovers_after_isolated_crash) {
    JitResult r = runAot(kValidProgram);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
