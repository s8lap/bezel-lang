// Codegen slice CG20 — throw + try/catch (§31.1–31.2, §31.4). Exceptions
// use an explicit-unwinding model: a module global holds the in-flight
// exception; `throw` stores it and branches to the enclosing catch
// dispatch (or the function's propagate path); a call that leaves the
// global set unwinds the same way. Catch arms match by the exception's
// runtime type (reusing the CG19 type descriptors), first match wins.
// Observable via the int exit code.
//
// User-defined exception classes are used: the built-in exception types
// (§31.4) have no runtime descriptor yet (deferred). Catch-arm variable
// binding (`catch var e: T`) and `when` guards are exercised below.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg20, throw_caught_by_type) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppError end AppError; "
        "class C public static method Main(): int "
        "  try throw new AppError(); catch AppError => return 42; end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_cg20, normal_path_skips_catch) {
    // No throw → the catch arm does not run; control falls past the try.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppError end AppError; "
        "class C public static method Main(): int "
        "  try return 7; catch AppError => return 42; end; "
        "  return 0; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_cg20, caught_by_base_type) {
    // Throw a subclass, catch by its (user-defined) base.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppError end AppError; "
        "class IoError : AppError end IoError; "
        "class C public static method Main(): int "
        "  try throw new IoError(); catch AppError => return 5; end; "
        "  return 0; "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_cg20, first_matching_arm_wins) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class A1 end A1; class A2 end A2; "
        "class C public static method Main(): int "
        "  try throw new A2(); "
        "  catch A1 => return 1; "
        "  catch A2 => return 2; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 2);
}

BEZEL_TEST(codegen_cg20, multi_type_arm) {
    // One arm listing several types matches any of them.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class A1 end A1; class A2 end A2; "
        "class C public static method Main(): int "
        "  try throw new A2(); catch A1, A2 => return 9; end; "
        "  return 0; "
        "end Main; end C;"), 9);
}

BEZEL_TEST(codegen_cg20, unmatched_arm_propagates_to_outer_try) {
    // Inner try doesn't match A2; it re-propagates to the outer try.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class A1 end A1; class A2 end A2; "
        "class C public static method Main(): int "
        "  try "
        "    try throw new A2(); catch A1 => return 1; end; "
        "  catch A2 => return 22; end; "
        "  return 0; "
        "end Main; end C;"), 22);
}

BEZEL_TEST(codegen_cg20, exception_propagates_across_calls) {
    // A callee throws; the caller's try catches after the call returns.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppError end AppError; "
        "class C "
        "  public static method Boom() throw new AppError(); end Boom; "
        "  public static method Main(): int "
        "    try Boom(); catch AppError => return 13; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 13);
}

BEZEL_TEST(codegen_cg20, rethrow_propagates) {
    // A bare `throw;` inside a catch arm re-raises to the outer try.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppError end AppError; "
        "class C public static method Main(): int "
        "  try "
        "    try throw new AppError(); catch AppError => throw; end; "
        "  catch AppError => return 99; end; "
        "  return 0; "
        "end Main; end C;"), 99);
}

BEZEL_TEST(codegen_cg20, code_after_catch_runs_on_normal_path) {
    // When the try completes normally, execution continues after the try.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppError end AppError; "
        "class C public static method Main(): int "
        "  var x := 0; "
        "  try x := 1; catch AppError => x := 2; end; "
        "  return x + 40; "
        "end Main; end C;"), 41);
}

// §31.2: a `catch var e: T` arm binds the caught exception to `e` (a
// borrow of the caught type), usable in the arm body and in a `when` guard.
BEZEL_TEST(codegen_cg20, catch_binding_in_body) {
    BEZEL_EXPECT_EQ(run(
        "module M; class E public Code: int := 0; end E; "
        "class C public static method Main(): int "
        "  try var e := new E(); e.Code := 7; throw e; "
        "  catch var ex: E => return ex.Code; end; return 0; "
        "end Main; end C;"), 7);
}

// §31.2: a `when` guard must be evaluated — a false guard skips the arm and
// the exception continues to the next arm (it is not silently caught).
BEZEL_TEST(codegen_cg20, catch_when_guard_false_skips_arm) {
    BEZEL_EXPECT_EQ(run(
        "module M; class E end E; "
        "class C public static method Main(): int "
        "  try throw new E(); "
        "  catch E when false => return 1; "
        "  catch E => return 9; end; return 0; "
        "end Main; end C;"), 9);
}

BEZEL_TEST(codegen_cg20, catch_guard_reads_binding) {
    // The guard reads the bound exception; the first matching guard wins.
    BEZEL_EXPECT_EQ(run(
        "module M; class E public Code: int := 0; end E; "
        "class C public static method Main(): int "
        "  try var e := new E(); e.Code := 2; throw e; "
        "  catch var ex: E when ex.Code = 1 => return 1; "
        "  catch var ex: E when ex.Code = 2 => return 7; "
        "  catch var ex: E => return 9; end; return 0; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_cg20, catch_when_guard_true_enters_arm) {
    BEZEL_EXPECT_EQ(run(
        "module M; class E end E; "
        "class C public static method Main(): int "
        "  try throw new E(); "
        "  catch E when true => return 5; "
        "  catch E => return 9; end; return 0; "
        "end Main; end C;"), 5);
}

// CG20: an exception thrown during construction (`new T()`) propagates to
// the surrounding try — from the constructor body itself, from a `base(...)`
// / `this(...)` chained constructor, and from an implicit base constructor.
BEZEL_TEST(codegen_cg20, exception_from_constructor_body) {
    BEZEL_EXPECT_EQ(run(
        "module M; class E end E; "
        "class N public constructor() throw new E(); end constructor; end N; "
        "class C public static method Main(): int "
        "  try var n := new N(); return 0; catch E => return 7; end; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_cg20, exception_from_base_constructor) {
    BEZEL_EXPECT_EQ(run(
        "module M; class E end E; "
        "class B public constructor() throw new E(); end constructor; end B; "
        "class D : B public constructor() base(); end constructor; end D; "
        "class C public static method Main(): int "
        "  try var d := new D(); return 0; catch E => return 5; end; "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_cg20, exception_from_this_chained_constructor) {
    BEZEL_EXPECT_EQ(run(
        "module M; class E end E; "
        "class P "
        "  public constructor() this(1); end constructor; "
        "  public constructor(x: int) throw new E(); end constructor; "
        "end P; "
        "class C public static method Main(): int "
        "  try var p := new P(); return 0; catch E => return 6; end; "
        "end Main; end C;"), 6);
}

BEZEL_TEST(codegen_cg20, exception_from_implicit_base_constructor) {
    BEZEL_EXPECT_EQ(run(
        "module M; class E end E; "
        "class B public constructor() throw new E(); end constructor; end B; "
        "class D : B end D; "
        "class C public static method Main(): int "
        "  try var d := new D(); return 0; catch E => return 4; end; "
        "end Main; end C;"), 4);
}
