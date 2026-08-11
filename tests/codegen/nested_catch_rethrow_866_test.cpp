// fix #866 / §31.2: a `throw` inside a catch arm that is caught by an INNER
// try/catch within the same arm wrongly freed the OUTER caught exception. The
// #397 optimization (a `throw <newExpr>` leaving a catch arm disposes the caught
// object, which is no longer referenced) fired even when the throw did not leave
// the arm but was caught locally by an inner try — so the outer caught exception
// was double-freed at the arm's own exit (a later `throw;` re-raise or a normal
// fall-through), crashing. The free is now gated on the throw's unwind target
// actually being the arm-leaving target.
//
// Value tests — runJit returns the raw int (kept in 0..255).

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

// The c31 repro: an inner try/catch (throwing Inn) inside the outer `catch Out`
// arm, then a bare `throw;` re-raising Out — caught by the enclosing `catch Out`
// (was SIGSEGV).
BEZEL_TEST(nested_catch_rethrow_866, inner_try_then_rethrow_outer) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Out: Exception end Out; class Inn: Exception end Inn; "
        "class C public static method Main(): int "
        "  try "
        "    try throw new Out(); "
        "    catch Out => do "
        "      try throw new Inn(); catch Inn => var y := 0; end; "
        "      throw; "
        "    end; end; "
        "  catch Out => return 42; end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// Single try (no enclosing handler): an inner throwing try/catch in the arm, then
// a normal fall-through must not double-free the caught Out.
BEZEL_TEST(nested_catch_rethrow_866, inner_try_then_fallthrough_no_double_free) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Out: Exception public static n: int := 0; "
        "  destructor Out.n := Out.n + 1; end destructor; end Out; "
        "class Inn: Exception end Inn; "
        "class C public static method Main(): int "
        "  var captured := -1; "
        "  try throw new Out(); "
        "  catch Out => do "
        "    try throw new Inn(); catch Inn => var y := 0; end; "
        "    captured := Out.n; "          // Out still alive -> 0
        "  end; end; "
        "  return captured; "              // 0
        "end Main; end C;"), 0);
}

// #397 preserved: a `throw <newExpr>` that genuinely LEAVES the arm still works
// (and frees the old caught exception) — the new exception is caught outside.
BEZEL_TEST(nested_catch_rethrow_866, throw_new_leaving_arm_preserved) {
    BEZEL_EXPECT_EQ(run(
        "module M; class A: Exception end A; class B: Exception end B; "
        "class C public static method Main(): int "
        "  try try throw new A(); catch A => throw new B(); end; "
        "  catch B => return 42; end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// Deeper nesting: an inner try-with-two-catches inside the arm, then `throw;`.
BEZEL_TEST(nested_catch_rethrow_866, deep_nested_then_rethrow) {
    BEZEL_EXPECT_EQ(run(
        "module M; class A: Exception end A; class B: Exception end B; class D: Exception end D; "
        "class C public static method Main(): int "
        "  try try throw new A(); "
        "    catch A => do "
        "      try try throw new B(); catch B => var b := 0; end; var c := 0; "
        "      catch D => var d := 0; end; "
        "      throw; "
        "    end; end; "
        "  catch A => return 42; end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// Regression: an inner try/catch that does NOT throw is unaffected, and a bare
// `throw;` after it still re-raises the outer exception.
BEZEL_TEST(nested_catch_rethrow_866, inner_no_throw_then_rethrow) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Out: Exception end Out; class Inn: Exception end Inn; "
        "class C public static method Main(): int "
        "  try try throw new Out(); "
        "    catch Out => do "
        "      try var z := 1; catch Inn => var y := 0; end; "
        "      throw; "
        "    end; end; "
        "  catch Out => return 42; end; "
        "  return 0; "
        "end Main; end C;"), 42);
}
