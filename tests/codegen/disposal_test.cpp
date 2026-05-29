// Codegen slice CG13 — `using var` scope-based disposal and the
// compiler-generated Dispose (§28.2–28.4). Each class gets a Dispose
// function that runs its destructor body; a `using var` is disposed at
// the end of its enclosing block, in reverse declaration order, on every
// normal-exit path (fall-through, `return`, `break`, `continue`). The
// destructor bumps a static counter so disposal is observable via the
// int exit code.
//
// (Idempotency, owned-field auto-disposal, base-class chaining, and the
// exception-unwind path are deferred to later slices; the tests use
// classes with a destructor and no owned fields or disposable base.)

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// A class C whose destructor increments the static counter _n, plus a
// Main body operating on it. Reused by most cases.
int runCounter(const std::string& mainBody) {
    return run(
        "module M; class C "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "  public static method Main(): int " + mainBody + " end Main; "
        "end C;");
}

} // namespace

BEZEL_TEST(codegen_cg13, using_var_disposed_at_do_block_end) {
    BEZEL_EXPECT_EQ(runCounter(
        "do using var x := new C(); end; "
        "return _n;"), 1);
}

BEZEL_TEST(codegen_cg13, not_yet_disposed_inside_block) {
    // Inside the do-block, before its end, the counter is still 0.
    BEZEL_EXPECT_EQ(runCounter(
        "var seen := -1; "
        "do using var x := new C(); seen := _n; end; "
        "return seen;"), 0);
}

BEZEL_TEST(codegen_cg13, two_using_vars_both_disposed) {
    BEZEL_EXPECT_EQ(runCounter(
        "do using var a := new C(); using var b := new C(); end; "
        "return _n;"), 2);
}

BEZEL_TEST(codegen_cg13, disposed_on_return_path) {
    // The using var lives in the method body; it is disposed when the
    // method returns (after the return value is evaluated).
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "  public static method Helper(): int "
        "    using var x := new C(); return 0; "
        "  end Helper; "
        "  public static method Main(): int "
        "    var r := Helper(); return _n + r; "
        "  end Main; "
        "end C;"), 1);
}

BEZEL_TEST(codegen_cg13, disposed_each_loop_iteration) {
    BEZEL_EXPECT_EQ(runCounter(
        "var i := 0; "
        "while i < 5 do using var x := new C(); i := i + 1; end; "
        "return _n;"), 5);
}

BEZEL_TEST(codegen_cg13, break_disposes_current_iteration) {
    // Three iterations create+dispose an instance; the third exits via
    // break, which still disposes that iteration's using var.
    BEZEL_EXPECT_EQ(runCounter(
        "var i := 0; "
        "while i < 100 do "
        "  using var x := new C(); "
        "  i := i + 1; "
        "  if i = 3 then break; end; "
        "end; "
        "return _n;"), 3);
}

BEZEL_TEST(codegen_cg13, continue_disposes_current_iteration) {
    // Every iteration creates a using var and continues; each is disposed.
    BEZEL_EXPECT_EQ(runCounter(
        "var i := 0; "
        "while i < 4 do "
        "  i := i + 1; "
        "  using var x := new C(); "
        "  if i > 0 then continue; end; "
        "end; "
        "return _n;"), 4);
}

BEZEL_TEST(codegen_cg13, nested_do_blocks_dispose_inner_first) {
    // After the inner block the counter is 1; after the outer it is 2.
    BEZEL_EXPECT_EQ(runCounter(
        "var afterInner := -1; "
        "do "
        "  using var outer := new C(); "
        "  do using var inner := new C(); end; "
        "  afterInner := _n; "
        "end; "
        "return afterInner * 10 + _n;"), 12);
}
