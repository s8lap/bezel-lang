// Spec-derived tests for issue #968 — the inline `out var` binding at a `new`
// (constructor) call site (§22.4).
//
// §22.4: `out var x` declares a new variable inline at the call site, typed from
// the callee's `out` parameter. This worked for METHOD calls (CallExpr) but not
// for CONSTRUCTOR calls (NewExpr): the sema arg-visiting path introduced the
// out-var LocalSymbol only for CallExpr arguments, so `new C(out var y)` reported
// "unresolved identifier y", and the codegen had no slot for `y` (it would read
// 0). THE FIX introduces the out-var in sema for NewExpr arguments (typed from
// the matching constructor's out parameter) and pre-creates its codegen slot
// (`ensureCtorOutVarSlots`) so the constructor's write is observed.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// The repro: `new C(out var y)` declares `y`, and the constructor writes 42
// through the out parameter — `y` reads 42 (was "unresolved identifier y", then
// a silent 0 before the codegen slot was wired).
BEZEL_TEST(ctor_out_var_968, out_var_at_new_receives_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public V: int; "
        "  public constructor(out n: int) n := 42; V := 1; end constructor; "
        "  public static method Main(): int "
        "    var c := new C(out var y); "
        "    c.Dispose(); "
        "    return y; "  // 42
        "  end Main; "
        "end C;"), 42);
}

// A mixed value + `out` constructor parameter list — the value arg and the
// out-var both bind. ctor sets n := a * 2 (a = 21) → y == 42.
BEZEL_TEST(ctor_out_var_968, mixed_value_and_out_var) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public V: int; "
        "  public constructor(a: int, out n: int) n := a * 2; V := a; end constructor; "
        "  public static method Main(): int "
        "    var c := new C(21, out var y); "
        "    c.Dispose(); "
        "    return y; "  // 42
        "  end Main; "
        "end C;"), 42);
}

// Regression: the inline `out var` at a METHOD call site is unaffected.
BEZEL_TEST(ctor_out_var_968, method_out_var_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method TryGet(out n: int): bool n := 7; return true; end TryGet; "
        "  public static method Main(): int "
        "    var ok := TryGet(out var y); "
        "    return y; "  // 7
        "  end Main; "
        "end C;"), 7);
}
