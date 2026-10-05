// fix #4641 (§33.3, v0.315): Main(): int return values exceeding 255 are
// truncated to 8 bits by the OS exit-status mechanism. The compiler now emits
// a diagnostic at the return statement.
//
// Boundary: 255 is the last valid exit code, 256 is the first rejected value.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// A well-formed program with exit code 255 (the boundary case). Must compile
// and run successfully.
constexpr const char* kExitCode255 =
    "module M; class C public static method Main(): int "
    "  return 255; "
    "end Main; end C;";

// A program that tries to return 256 — the first value that truncates to a
// different exit code (256 mod 256 = 0). Must be rejected with a diagnostic
// about the exit-code limit.
constexpr const char* kExitCode256 =
    "module M; class C public static method Main(): int "
    "  return 256; "
    "end Main; end C;";

// A program that returns 301 — a value with a non-trivial truncation pattern
// (301 mod 256 = 45). Must be rejected with a diagnostic showing the truncation.
constexpr const char* kExitCode301 =
    "module M; class C public static method Main(): int "
    "  return 301; "
    "end Main; end C;";

// Control: a program returning 200 (well within range). Must compile and run.
constexpr const char* kExitCode200 =
    "module M; class C public static method Main(): int "
    "  return 200; "
    "end Main; end C;";

// Helper: check if diagnostics contain a substring.
bool diagContains(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// Was this JitResult a SKIP (toolchain unavailable) rather than a real failure?
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

}  // namespace

// fix #4641-1: exit code 255 (the boundary) compiles cleanly and exits 255.
BEZEL_TEST(main_exit_code_limit_4641, exit_255_boundary) {
    JitResult r = runAot(kExitCode255);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 255);
}

// fix #4641-2: exit code 256 is rejected with a diagnostic about the limit.
BEZEL_TEST(main_exit_code_limit_4641, exit_256_rejected) {
    JitResult r = runAot(kExitCode256);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // The diagnostic must mention the 8-bit exit code limit.
    BEZEL_EXPECT_TRUE(diagContains(r, "8-bit"));
    // And the value 256.
    BEZEL_EXPECT_TRUE(diagContains(r, "256"));
}

// fix #4641-3: exit code 301 is rejected, and the diagnostic shows truncation.
BEZEL_TEST(main_exit_code_limit_4641, exit_301_rejected) {
    JitResult r = runAot(kExitCode301);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // The diagnostic must mention the 8-bit limit and value 301.
    BEZEL_EXPECT_TRUE(diagContains(r, "301"));
    BEZEL_EXPECT_TRUE(diagContains(r, "8-bit"));
}

// fix #4641-4: control — exit code 200 compiles and runs successfully.
BEZEL_TEST(main_exit_code_limit_4641, exit_200_control) {
    JitResult r = runAot(kExitCode200);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}
