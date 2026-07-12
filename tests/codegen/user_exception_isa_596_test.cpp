// Codegen tests for issue #596 — `catch var e: Exception`, `is Exception`,
// and `as Exception` do NOT match a user-defined class that inherits from a
// built-in exception base (e.g. `class ErrA: Exception`).
//
// Root cause: the runtime type-descriptor for a user exception class (Pass B/C
// in declareVtables) only recorded user-type ancestors via
// collectTypeAndBases (which walks TypeSymbol::baseTypes).  Built-in exception
// types have NO TypeSymbol (declaration == nullptr) and are absent from
// TypeSymbol::baseTypes, so `td$ErrA` was `[td$ErrA, null]` — it did not
// include `btd$Exception`.  emitIsA walked that array and never found the
// builtin descriptor → catch/is/as all missed the match.
//
// Fix: Pass B now also counts the builtin ancestor chain length, Pass C
// appends the builtinDesc_ globals for each ancestor.  After the fix,
// `td$ErrA` = `[td$ErrA, btd$Exception, null]`, so emitIsA succeeds.
//
// Tests:
//   Group 1. `catch var e: Exception` catches a user-exception derived from Exception.
//   Group 2. `is Exception` returns true for a user-exception instance.
//   Group 3. `as Exception` yields a non-null value for a user-exception instance.
//   Group 4. Two-level user hierarchy caught by the builtin base (ErrB: ErrA: Exception).
//   Group 5. Catch ordering: specific arm before Exception arm still works.
//   Group 6. Regression: catching the exact user type still works.
//   Group 7. Regression: ordinary user-to-user `is`/`as` still works.
//   Group 8. Regression: built-in exception caught as its own builtin type.
//
// per §12 (inheritance), §21 (is/as), §31 (catch matching).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// ── Group 1: `catch var e: Exception` catches user-exception derived from Exception ──

// §31 / §12: throw a user exception whose class derives from Exception; catch
// arm typed as `Exception` must match (the user-exception IS-A Exception).
// Before fix: catch arm not reached → exits 0.  After fix: caught → exits 1.
// per §31.2 (first matching arm wins), §12 (user class inherits builtin base).
BEZEL_TEST(codegen_596, catch_as_builtin_base_exception_spec_31_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrA; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrA(\"hi\"); "
        "  catch var e: Exception => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        1);
}

// §31 / §12: catch arm typed `Exception` on an ErrA: Exception — no-arg ctor.
// per §31.2 / §12.
BEZEL_TEST(codegen_596, catch_as_builtin_base_exception_noarg_spec_31_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor() end constructor; "
        "end ErrA; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrA(); "
        "  catch var e: Exception => return 7; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        7);
}

// §31 / §12: user exception deriving from ArgumentException (a non-root
// builtin exception), caught as ArgumentException.
// per §31.2 / §12.
BEZEL_TEST(codegen_596, catch_as_builtin_mid_base_argument_exception_spec_31_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrArg: ArgumentException "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrArg; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrArg(\"bad\"); "
        "  catch var e: ArgumentException => return 2; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        2);
}

// §31 / §12: user exception deriving from ArgumentException, caught as the
// top-level Exception base.
// per §31.2 / §12 (transitive builtin ancestor).
BEZEL_TEST(codegen_596, catch_as_top_builtin_base_transitive_spec_31_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrArg: ArgumentException "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrArg; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrArg(\"bad\"); "
        "  catch var e: Exception => return 3; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        3);
}

// ── Group 2: `is Exception` returns true for a user-exception instance ────────

// §21: `obj is Exception` must hold when obj's runtime type derives from
// Exception.  Observable: return 1 if true, 0 if false.
// Before fix: returns 0.  After fix: returns 1.
// per §21 (type test), §12 (inheritance).
BEZEL_TEST(codegen_596, is_builtin_base_exception_spec_21_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor() end constructor; "
        "end ErrA; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrA(); "
        "  catch var e: ErrA => "
        "    return (1 if e is Exception else 0); "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        1);
}

// §21: `is not Exception` returns false for a user-exception instance.
// per §21 / §12.
BEZEL_TEST(codegen_596, is_not_builtin_base_exception_spec_21_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor() end constructor; "
        "end ErrA; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrA(); "
        "  catch var e: ErrA => "
        "    return (1 if e is not Exception else 0); "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        0);
}

// ── Group 3: `as Exception` yields a non-null value ──────────────────────────

// §21: `obj as Exception` must yield a non-null value (the same pointer)
// when obj's runtime type derives from Exception.
// per §21 / §12.
BEZEL_TEST(codegen_596, as_builtin_base_exception_nonnull_spec_21_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor() end constructor; "
        "end ErrA; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrA(); "
        "  catch var e: ErrA => "
        "    return (1 if (e as Exception) is not null else 0); "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        1);
}

// ── Group 4: Two-level user hierarchy caught by the builtin base ──────────────

// §12 / §31: ErrB: ErrA: Exception — throw ErrB, catch as Exception.
// Before fix: catch arm not reached.  After fix: caught.
// per §31.2 / §12 (multi-level inheritance through user chain and builtin base).
BEZEL_TEST(codegen_596, two_level_user_exception_caught_as_builtin_base_spec_31_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrA; "
        "class ErrB: ErrA "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrB; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrB(\"deep\"); "
        "  catch var e: Exception => return 4; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        4);
}

// §12 / §31 / §21: ErrB: ErrA: Exception — `ErrB instance is Exception` == true.
// per §21 / §12 (transitive builtin ancestor through user chain).
BEZEL_TEST(codegen_596, two_level_user_is_builtin_base_spec_21_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrA; "
        "class ErrB: ErrA "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrB; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrB(\"chain\"); "
        "  catch var e: ErrB => "
        "    return (1 if e is Exception else 0); "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        1);
}

// ── Group 5: Catch ordering — specific arm before Exception arm ───────────────

// §31.2.3: when both `catch var e: ErrA` and `catch var e: Exception` exist,
// the more specific arm fires first.  After fix the Exception arm also works,
// but the ordering must still be respected.
// per §31.2.3 (first matching arm wins).
BEZEL_TEST(codegen_596, catch_ordering_specific_before_base_spec_31_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor() end constructor; "
        "end ErrA; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrA(); "
        "  catch var e: ErrA => return 10; "
        "  catch var e: Exception => return 20; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        10);  // specific arm fires, not the base
}

// §31.2.3: Exception arm fires when the specific type doesn't match.
// per §31.2.3.
BEZEL_TEST(codegen_596, catch_ordering_base_arm_fires_for_other_subclass_spec_31_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor() end constructor; "
        "end ErrA; "
        "class ErrB: Exception "
        "  public constructor() end constructor; "
        "end ErrB; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrB(); "
        "  catch var e: ErrA => return 10; "
        "  catch var e: Exception => return 20; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        20);  // ErrA arm misses, Exception arm fires
}

// ── Group 6: Regression — catching the exact user type still works ────────────

// Regression: throw/catch on the exact user-exception type must still work
// after the builtin-chain fix is applied to descriptors.
// per §31 / §12.
BEZEL_TEST(codegen_596, regression_catch_exact_user_type_spec_31) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrA; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrA(\"ok\"); "
        "  catch var e: ErrA => return e.Message.Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        2);  // "ok".Length = 2
}

// ── Group 7: Regression — ordinary user-to-user `is`/`as` still works ─────────

// Regression: user-to-user `is` test must continue to work (no regression
// from adding builtin descriptors to the chain).
// per §21 / §12 (user-only inheritance).
BEZEL_TEST(codegen_596, regression_user_to_user_is_spec_21_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal end Animal; "
        "class Dog: Animal end Dog; "
        "class C public static method Main(): int "
        "  var d := new Dog(); "
        "  return (1 if d is Animal else 0); "
        "end Main; end C;"),
        1);
}

// Regression: `as` on user-to-user hierarchy.
// per §21 / §12.
BEZEL_TEST(codegen_596, regression_user_to_user_as_spec_21_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal end Animal; "
        "class Dog: Animal end Dog; "
        "class C public static method Main(): int "
        "  var d := new Dog(); "
        "  var a := d as Animal; "
        "  return (1 if a is not null else 0); "
        "end Main; end C;"),
        1);
}

// ── Group 8: Regression — built-in exception caught as its own builtin type ───

// Regression: built-in exception throw/catch must still work after the fix.
// per §31.
BEZEL_TEST(codegen_596, regression_builtin_exception_catch_still_works_spec_31) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ArgumentException(\"x\"); "
        "  catch var e: ArgumentException => return 5; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        5);
}

// Regression: builtin exception caught as Exception base.
// per §31 / §12 (builtin hierarchy).
BEZEL_TEST(codegen_596, regression_builtin_caught_as_top_exception_spec_31) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ArgumentException(\"x\"); "
        "  catch var e: Exception => return 6; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        6);
}
