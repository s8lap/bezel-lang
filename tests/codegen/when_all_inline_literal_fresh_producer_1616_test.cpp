// Codegen / runtime — issue #1616 (spec §30.10) accept control. The crash
// half of #1616 was fixed by `lowerTaskArrayArg` mirroring the WhenAny
// path; the named-`var` (i.e. non-`new`) bound path is the only fresh-
// producer shape that still passes the new REJECT (`borrowing_task_array_
// fresh_producer_1616.bzl`). A `var x := Probe.Tick(...);` puts the task
// in a NAMED binding, so its lifetime is the binding's scope (not the
// full expression). A subsequent `Task.WhenAll([x, y])` borrows the
// bound tasks; the bindings are still in scope and disposed at scope
// end. This test pins that bound-producer path under the new lint.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

const char* kProbe =
    "class Probe "
    "  public static async method Tick(): Task "
    "    await Task.Delay(0); "
    "  end Tick; "
    "end Probe; ";

}  // namespace

// #1616: bound `var` Task producers over WhenAll. Compile-clean under
// the fresh-producer lint (each task is in a named local), no LSan
// reports at -O0 (each task reclaims at the binding's scope end via
// the using-var disposal path).
BEZEL_TEST(when_all_inline_literal_fresh_producer_1616,
           when_all_named_local_producers_lint_clean_spec_30_10) {
    const std::string src = std::string("module M; ") + kProbe +
        "class C "
        "  public static method Main(): int "
        "    using var a := Probe.Tick(); "
        "    using var b := Probe.Tick(); "
        "    using var w := Task.WhenAll([a, b]); "
        "    w.Wait(); "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}
