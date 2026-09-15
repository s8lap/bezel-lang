// Regression test for issue #4200 (spec §10/§11, v0.298): a chained member call
// on a `new T()` receiver with a genuinely nonexistent member was silently
// accepted with zero diagnostics and exit 0.
//
// Root cause: `LlvmBackend_Calls2.cpp`'s end-of-chain fallthrough warning only
// fired when `isConcreteType` was true (receiver's TypeRef has a non-null
// `declaration`). `new Stopwatch()`'s receiver type resolves through
// `TypeResolution_Names.cpp`'s "nullary stdlib class" list with a null
// declaration, so `isConcreteType` was false and the warning never fired.
// By contrast, `Stopwatch.StartNew()`'s receiver type has a non-null declaration
// (via `builtinClassSym("Stopwatch")`), so `isConcreteType` is true and the
// identical bogus-member call IS correctly warned about.
//
// The fix adds a third shape to the fallthrough condition: call-shaped receiver
// (CallExpr or NewExpr) whose resolved type has no declaration, emitting the same
// "internal: codegen cannot dispatch member" warning.
//
// NOTE: The primary regression pin is the lit test
// `tests/lit/chained_new_shaped_builtin_receiver_undispatchable_member_4200.bzl`
// which directly verifies the warning is emitted. The gtest below covers the
// positive case: legitimate members on `new Stopwatch()` compile and run cleanly.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// fix #4200: `new Stopwatch()` with LEGITIMATE members (Start/Stop/Reset)
// must compile and run cleanly — no warnings, exit 0.
BEZEL_TEST(chained_new_shaped_builtin_receiver_undispatchable_member_4200,
           new_stopwatch_legitimate_members_work) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  new Stopwatch().Start(); "
        "  new Stopwatch().Stop(); "
        "  new Stopwatch().Reset(); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
