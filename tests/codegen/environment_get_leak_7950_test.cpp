// Regression tests for issue #7950 — the `Environment.Get` facet: the fresh
// `string` it produces was never discharged, so every call leaked the string
// header AND its data copy.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §2.4 (v0.355) declares:
//
//     public static method Get(name: string): string?;    // null when the
//                                                        //   variable is
//                                                        //   absent
//
// and the `Environment` arm of `tryLowerBuiltinClassCall`
// (`src/codegen/LlvmBackend_Calls.cpp`) mints that result the way every fresh
// string production is minted: a malloc'd COPY of the variable's bytes plus a
// malloc'd `bz.string` header whose rc is set to 1 (`initStringRc`).  The copy
// is NECESSARY — the C library owns the buffer `getenv` returns and may
// invalidate it on the next `setenv`, so the Bezel string cannot borrow it.
//
// §28.3 ("Fresh productions are owned") and §28.9 ("the implementation
// reclaims the production") therefore make the CALLER the owner of exactly ONE
// reference, to be discharged exactly once.
//
// THE DEFECT.  Nothing recognised the production, so no consumer discharged
// it:  a bound local was not even a release candidate (the local's own type is
// the call's sema type, and `Environment.Get` has none — the §2.4 `Get` result
// is UNTYPED in sema, so the borrow arm of `collectStringReleaseEligible` does
// not admit it either), and a call argument was never released at the end of
// the full expression.
//
// MEASURED pre-fix (base b0325c3c, generated-code LeakSanitizer, a
// 100-iteration loop of `var g := Environment.Get("BZ_T_7950_X");`):
// 200 allocations / 3400 B — 100 headers (32 B) + 100 data buffers.
//
// THE FIX.  `isPlusOneStringProducer` (`src/codegen/LlvmBackend_Setup.cpp`)
// gains the `Environment.Get` arm — the same treatment the `Path.*` family
// (#7865) and `File.ReadAllText` (#8016) got.  A bound local then TRANSFERS the
// single reference (no extra retain) and the scope-end release drops rc 1->0
// exactly once; a call argument is discharged at the end of the full
// expression.  No double free: an ESCAPING local is excluded from the
// scope-end release by the escape walk (its +1 then leaks — the safe
// direction, and the direction every other producer arm takes).
//
// ORACLES.  Every row runs the real AOT path under the GENERATED-CODE
// sanitizer (`runAotChecked`: `opts.sanitizeAddress = true`), so a leak shows
// up as a LeakSanitizer report and a non-zero exit code — not as a wrong
// value.  The loop is 100 iterations, because a one-shot leak can hide under
// the sanitizer's noise.
//
// Hermetic by construction: each test program is its own forked process, so
// `Set` mutates only that process's environment, and every variable name is
// prefixed `BZ_T_7950_` to avoid colliding with the harness's own.
//
// Cite: stdlib §2.4, §4.6, language spec §28.3, §28.9, v0.355.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::aot_detail::hasSanitizerReport;
using bezel::test::runAotChecked;

namespace {

std::string prog(const std::string& body) {
    return "module Conf;\npublic class Program\n"
           "  public static method Main()\n" + body +
           "  end Main;\nend Program;\n";
}

}  // namespace

// The issue's own shape: the result is BOUND to a local, 100 times.  Before
// the fix this row failed with a LeakSanitizer report (200 allocations / 3400
// B); after it, the program exits 0 with no sanitizer report.
BEZEL_TEST(environment_get_leak_7950, environment_get_bound_to_a_local_is_discharged) {
    const std::string p = prog(
        "    Environment.Set(\"BZ_T_7950_X\", \"v\");\n"
        "    var n := 0;\n"
        "    for var i in 0..<100 do\n"
        "      var g := Environment.Get(\"BZ_T_7950_X\");\n"
        "      if g <> null then n := n + 1; end if;\n"
        "    end for;\n"
        "    Console.WriteLine(n);");
    auto r = runAotChecked(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasSanitizerReport(r.stdoutText));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

// The other consumer shape: the result is a CALL ARGUMENT, so it is
// discharged at the end of the full expression rather than at a scope end.
// Before the fix this leaked the same 2 allocations per call.
BEZEL_TEST(environment_get_leak_7950, environment_get_as_an_argument_is_discharged) {
    const std::string p = prog(
        "    Environment.Set(\"BZ_T_7950_Y\", \"w\");\n"
        "    for var i in 0..<100 do\n"
        "      Console.WriteLine(Environment.Get(\"BZ_T_7950_Y\") ?? \"none\");\n"
        "    end for;");
    auto r = runAotChecked(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasSanitizerReport(r.stdoutText));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string expected;
    for (int i = 0; i < 100; ++i) expected += "w\n";
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// The POSITIVE CONTROL: the same loop shape with NO `Get` in it.  It is clean
// on both arms, which is what makes the two rows above a measurement of the
// `Get` production rather than of the loop.
BEZEL_TEST(environment_get_leak_7950, environment_set_only_loop_does_not_leak) {
    const std::string p = prog(
        "    for var i in 0..<100 do\n"
        "      Environment.Set(\"BZ_T_7950_Z\", \"v\");\n"
        "    end for;\n"
        "    Console.WriteLine(\"done\");");
    auto r = runAotChecked(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasSanitizerReport(r.stdoutText));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "done\n");
}
