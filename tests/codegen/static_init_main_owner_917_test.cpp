// fix #917 / #918 / §22.13: invoking the entry point Main IS the first use of
// its declaring class, so that class's static fields must be initialized before
// Main's body runs ("once initialization begins for a type, all of its static
// fields are initialized before any of its methods can run").
//
// Before the fix, codegen skipped triggering Main's own declaring class's lazy
// static init (the `!isMain` guard in emitEnsureStaticInit's call site,
// LlvmBackend_Setup.cpp). A static field initializer with observable side
// effects on its enclosing class therefore never ran unless Main's body happened
// to read that class's own static state — so #917 returned 100 (Widget never
// constructed) instead of 101, and #918 returned 0 (the two static owned Cells
// never constructed) instead of 2.
//
// These are codegen tests (static-init timing), so runJit is the right harness —
// the exit code captures the value returned from Main after static init.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #917 / §22.13: Widget.total's field initializer (= 100) must complete before
// any Widget ctor runs; C's static field `w := new Widget()` (ctor increments
// total) runs because invoking Main is first-use of C. Expected: 101.
BEZEL_TEST(static_init_main_owner_917, own_class_static_field_init_runs_before_main) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Widget "
        "  public static total: int := 100; "
        "  public constructor() Widget.total := Widget.total + 1; end constructor; "
        "end Widget; "
        "class C "
        "  public static readonly w: Widget := new Widget(); "
        "  public static method Main(): int "
        "    return Widget.total; "
        "  end Main; "
        "end C;"), 101);
}

// #918 / §22.13: two `static owned` Cell fields on Main's declaring class are
// constructed at first-use of C (Main's invocation), so Cell.n == 2 inside
// Main's body. (The static owned cells are disposed at program exit, not before
// Main returns, so the returned value observes them live.) Expected: 2.
BEZEL_TEST(static_init_main_owner_917, own_class_static_owned_fields_constructed_before_main) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cell "
        "  public static n: int := 0; "
        "  public constructor() Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        // §22.12 rule 2 (v0.163, #1363): mutable static class fields are
        // errors; `readonly` is required. Static initializers still run at
        // first-use so Cell.n == 2 when Main executes (§22.13).
        "class C "
        "  public static owned readonly c1: Cell := new Cell(); "
        "  public static owned readonly c2: Cell := new Cell(); "
        "  public static method Main(): int "
        "    return Cell.n; "
        "  end Main; "
        "end C;"), 2);
}

// NOTE: the dual of these cases — a throwing static field initializer ON MAIN'S
// OWN class now fails *before* Main's body runs (§22.13: init precedes the
// method), so a try/catch INSIDE Main cannot catch its own class's init failure;
// the TypeInitializationException is unhandled at the entry point and the
// program aborts with the unhandled-exception exit code (70). That is NOT
// expressible as a runJit test — an unhandled exception in JIT'd code calls the
// runtime's process-level handler, which exits the whole test harness. It is
// verified out-of-band by compiling the repro to an executable (exit 70).

// Control: a throwing static initializer on a DIFFERENT class, accessed inside a
// try in Main's body, is still catchable at the access point (unchanged by the
// fix — Main's own class C here has no static field state). Expected: 42.
BEZEL_TEST(static_init_main_owner_917, other_class_failed_init_still_caught_at_access) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Boom public static _x: int := C.Fail(); end Boom; "
        "class C "
        "  public static method Fail(): int => throw new Exception(\"boom\"); "
        "  public static method Main(): int "
        "    try "
        "      var v := Boom._x; "
        "      return v; "
        "    catch Exception => return 42; "
        "    end; "
        "  end Main; "
        "end C;"), 42);
}
