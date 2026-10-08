// Regression test for #5441: string leaked when +1 string producer passed as
// borrowed arg to closure call or IIFE.
//
// THE DEFECT: When a direct +1 string producer (concat, interpolation, etc.)
// is passed as a borrowed string argument to a closure call or IIFE, the
// borrowed-string pin logic emitted retain (RC=1→2) + release (RC=2→1).
// Since the concat result has RC=1 (owned by the caller's scope), the
// retain+release left RC at 1, and the string was never freed.
//
// THE FIX: For a direct +1 producer, emit only the post-call release
// (no retain: RC=1→0, string freed). This is correct because the callee
// is a borrow — it does not retain. The caller's +1 is the only reference,
// and the post-call release drops it. RC=1 with release-only → RC=0 is
// safe precisely because the callee does not retain the borrowed param.
//
// For literals (sentinel RC), the existing behavior is preserved.
//
// SPEC: §4.6 / §28.9 / §28.3 (v0.326).
//
// FAIL-WITHOUT: 500 iterations × 32 B = 16000 B direct + 1500 B indirect.
// FAIL-WITH: LSan clean, exit 0.
//
// LOOP AMPLIFICATION: per the leak-testing vacuity guidance — 500 iterations
// × 32 B = 16000 B is well past LSan noise floor.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Like runAotChecked but returns: 0=clean, 1=leak, -1=infra skip
int checkLeak(const std::string& source) {
    auto r = runAotChecked(source);
    if (!r.ran) {
        auto& diags = r.diagnostics.diagnostics();
        for (auto& d : diags) {
            if (d.message.find("sanitizer fired") != std::string::npos)
                return 1;  // leak detected
        }
        return -1;  // some other infra issue
    }
    return 0;  // clean
}

// Returns exit code or -1 on infra failure
int runExit(const std::string& source) {
    auto r = runAot(source);
    return r.ran ? r.exitCode : -1;
}

// ---- Shape 1: named closure, concat arg ---------------------------------
const std::string kNamedClosureConcat =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    for var i in 0..<500 do "
    "      var f := lambda(s: string) => s.Length; "
    "      var n := f(\"a\" + \"b\"); "
    "      if n <> 2 then return 1; end; "
    "    end for; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---- Shape 2: IIFE, concat arg -----------------------------------------
const std::string kIIFEConcat =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    for var i in 0..<500 do "
    "      var n := (lambda(s: string) => s.Length)(\"a\" + \"b\"); "
    "      if n <> 2 then return 1; end; "
    "    end for; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---- Control: literal string arg -----------------------------------------
const std::string kNamedClosureLiteral =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    for var i in 0..<500 do "
    "      var f := lambda(s: string) => s.Length; "
    "      var n := f(\"ab\"); "
    "      if n <> 2 then return 1; end; "
    "    end for; "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// ===========================================================================
// PRIMARY ROWS: named closure and IIFE with concat arg.
// Pre-fix: 16000 B direct + 1500 B indirect leaked over 500 iterations.
// Post-fix: LSan clean, exit 0.
// ===========================================================================
BEZEL_TEST(closure_call_string_arg_leak_5441,
           named_closure_concat_arg_4_6_28_9) {
    int exitCode = runExit(kNamedClosureConcat);
    BEZEL_EXPECT_EQ(exitCode, 0);
    int leak = checkLeak(kNamedClosureConcat);
    if (leak == -1) return;  // infra skip
    BEZEL_EXPECT_EQ(leak, 0);  // 0=clean, 1=leak
}

BEZEL_TEST(closure_call_string_arg_leak_5441,
           iife_concat_arg_4_6_28_9) {
    int exitCode = runExit(kIIFEConcat);
    BEZEL_EXPECT_EQ(exitCode, 0);
    int leak = checkLeak(kIIFEConcat);
    if (leak == -1) return;  // infra skip
    BEZEL_EXPECT_EQ(leak, 0);
}

// ===========================================================================
// CONTROL: literal arg should still work (sentinel RC, no leak possible).
// ===========================================================================
BEZEL_TEST(closure_call_string_arg_leak_5441,
           control_named_closure_literal_arg_4_6) {
    int exitCode = runExit(kNamedClosureLiteral);
    BEZEL_EXPECT_EQ(exitCode, 0);
    int leak = checkLeak(kNamedClosureLiteral);
    if (leak == -1) return;  // infra skip
    BEZEL_EXPECT_EQ(leak, 0);
}
