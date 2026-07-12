// Codegen slice CG20 — throw + try/catch (§31.1–31.2, §31.4). Exceptions
// use an explicit-unwinding model: a module global holds the in-flight
// exception; `throw` stores it and branches to the enclosing catch
// dispatch (or the function's propagate path); a call that leaves the
// global set unwinds the same way. Catch arms match by the exception's
// runtime type (reusing the CG19 type descriptors), first match wins.
// Observable via the int exit code.
//
// Both user-defined exception classes and the built-in exception types
// (§31.4: Exception, ArgumentException, OverflowException, …) are tested.
// Catch-arm variable binding (`catch var e: T` or `catch e: T`) and
// `when` guards are exercised below.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
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
        "  try throw new A2(); catch A1 | A2 => return 9; end; "
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

// ── Built-in exception types (§31.4) ──────────────────────────────────────
// Prior to this fix, `new Exception()` produced a null pointer (the type
// had no user TypeSymbol), so the catch-dispatch emitIsA always failed.
// These tests confirm that built-in exception objects are properly
// heap-allocated with the correct vtable so the hierarchy check works.

BEZEL_TEST(codegen_cg20, builtin_exception_throw_catch) {
    // §31.4: throw a built-in Exception; catch by the same type.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 0; "
        "  try throw new Exception(); "
        "  catch var e: Exception => r := 42; end; "
        "  return r; "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_cg20, builtin_exception_catch_base) {
    // §31.4: throw ArgumentException; catch by its base Exception.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 0; "
        "  try throw new ArgumentException(); "
        "  catch var e: Exception => r := 7; end; "
        "  return r; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_cg20, builtin_exception_hierarchy_match) {
    // §31.4: OverflowException → ArithmeticException → Exception chain.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 0; "
        "  try throw new OverflowException(); "
        "  catch var e: ArithmeticException => r := 5; end; "
        "  return r; "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_cg20, builtin_exception_no_match_falls_through) {
    // §31.4: throw ArgumentException; a DivideByZeroException arm doesn't
    // match; the exception propagates to the outer try that does match.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 0; "
        "  try "
        "    try throw new ArgumentException(); "
        "    catch var e: DivideByZeroException => r := 99; end; "
        "  catch var e: Exception => r := 3; end; "
        "  return r; "
        "end Main; end C;"), 3);
}

// ── catch without var (parser recovery — `catch e: T` = `catch var e: T`) ─

BEZEL_TEST(codegen_cg20, catch_binding_without_var) {
    // `catch e: Exception` is accepted as a binding pattern even without
    // the `var` keyword (parser tolerates the common C#/Java idiom).
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 0; "
        "  try throw new Exception(); "
        "  catch e: Exception => r := 11; end; "
        "  return r; "
        "end Main; end C;"), 11);
}

// ── Exception.Message property ─────────────────────────────────────────────

// §31 built-in exception objects now carry a message string at byte offset 8.
// Accessing .Message returns it (or empty string if no-arg construction).

BEZEL_TEST(codegen_cg20, exception_message_property) {
    // ArgumentException constructed with a message string.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    throw new ArgumentException(\"bad arg\"); "
        "  catch var e: ArgumentException => return e.Message.Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        7);   // "bad arg".Length = 7
}

BEZEL_TEST(codegen_cg20, exception_message_no_arg) {
    // No-arg exception construction: Message should be empty string, Length 0.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    throw new InvalidOperationException(); "
        "  catch var e: InvalidOperationException => return e.Message.Length; "
        "  end; "
        "  return -1; "
        "end Main; end C;"),
        0);
}

// ── Array bounds checking ──────────────────────────────────────────────────

BEZEL_TEST(codegen_cg20, array_bounds_oor_read_throws) {
    // Reading past the end of an inline array raises IndexOutOfRangeException.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [5]int; "
        "  try "
        "    var v := a[10]; return v; "
        "  catch var e: IndexOutOfRangeException => return 99; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        99);
}

BEZEL_TEST(codegen_cg20, array_bounds_oor_write_throws) {
    // Writing past the end also raises.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [3]int; "
        "  try "
        "    a[5] := 42; return 0; "
        "  catch var e: IndexOutOfRangeException => return 77; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        77);
}

// ── Multiple catch arms ────────────────────────────────────────────────────

BEZEL_TEST(codegen_cg20, multiple_catch_arms_comma_separated) {
    // §31.2: Multiple catch arms under a single `catch` keyword,
    // comma-separated. First matching arm runs.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var caught := 0; "
        "  try "
        "    throw new IndexOutOfRangeException(); "
        "  catch "
        "    var e: ArgumentException => caught := 1;, "
        "    var e: IndexOutOfRangeException => caught := 2;, "
        "    var e: Exception => caught := 3; "
        "  end; "
        "  return caught; "
        "end Main; end C;"),
        2);
}

BEZEL_TEST(codegen_cg20, type_test_in_interpolated_string) {
    // §18.5 + §12.7: `{x is T}` inside an interpolated string must resolve
    // the type node; it was previously skipped by TypeResolution.
    // Returns 1 if all three type tests format correctly, 0 otherwise.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IA end IA; "
        "class A : IA destructor end destructor; end A; "
        "class B : IA destructor end destructor; end B; "
        "class C public static method Main(): int "
        "  using var a := new A(); "
        "  using var b := new B(); "
        "  var s := $\"{a is IA},{b is IA},{a is B}\"; "
        // #452: bool.ToString() → "True"/"False" per stdlib; is-expressions yield bool.
        "  if s = \"True,True,False\" then return 1; else return 0; end; "
        "end Main; end C;"),
        1);
}

// #720 B2 / §31: a `when` guard that ALLOCATES (or calls a method) must run
// correctly even though an exception is in flight during catch dispatch. Before
// the fix the in-flight exception sat in the global slot, so the guard's `new`
// misfired the pending-exception check (freeing the fresh object, treating the
// guard's own op as a failure) and the whole construct went unhandled.
BEZEL_TEST(codegen_cg20, when_guard_allocation_does_not_misfire_720) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppErr end AppErr; "
        "class Helper public v: int; "
        "  public constructor(x: int) v := x; end constructor; end Helper; "
        "class C "
        "  public static method CheckIt(): bool "
        "    using var h := new Helper(7); return h.v = 7; "
        "  end CheckIt; "
        "  public static method Main(): int "
        "    try throw new AppErr(); "
        "    catch AppErr when C.CheckIt() => return 42; "
        "    catch AppErr => return 99; "
        "    end; "
        "    return 0; "
        "  end Main; end C;"),
        42);
}

// #720 B2 / §31: a `throw` from a `when` guard supersedes the in-flight
// exception and propagates to the ENCLOSING handler (consistent with
// throw-in-finally, #720 B1). The inner guard throws GuardErr; the outer
// `catch GuardErr` catches it.
BEZEL_TEST(codegen_cg20, when_guard_throw_propagates_to_enclosing_720) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppErr end AppErr; "
        "class GuardErr end GuardErr; "
        "class C "
        "  public static method Boom(): bool throw new GuardErr(); end Boom; "
        "  public static method Main(): int "
        "    try "
        "      try throw new AppErr(); "
        "      catch AppErr when C.Boom() => return 10; "
        "      end; "
        "    catch GuardErr => return 20; "
        "    end; "
        "    return 0; "
        "  end Main; end C;"),
        20);
}

// #720 B2 / §31: a guard that does NOT match falls through to the next arm with
// the original in-flight exception intact (the set-aside exception is restored
// after the guard runs).
BEZEL_TEST(codegen_cg20, when_guard_false_restores_inflight_exception_720) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppErr end AppErr; "
        "class C "
        "  public static method No(): bool return false; end No; "
        "  public static method Main(): int "
        "    try throw new AppErr(); "
        "    catch AppErr when C.No() => return 1; "
        "    catch AppErr => return 77; "  // original AppErr still matches here
        "    end; "
        "    return 0; "
        "  end Main; end C;"),
        77);
}

// ── #739 / §31.1: the thrown value must be an exception (a class instance) ────
//
// `throw` constructs and raises an exception class instance (§31.1). Codegen
// stores the thrown value into the in-flight-exception global as an exception
// object pointer and dispatches on its runtime type descriptor; a non-class
// value (primitive, string, record, enum, function) has no such descriptor, so
// throwing one used to miscompile into a SIGSEGV. Sema now rejects it with a
// clean diagnostic at the throw site instead of producing a crashing binary.

namespace {
// True iff at least one diagnostic message mentions the throwable-type rule.
bool rejectsNonThrowable(const JitResult& r) {
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("must be an exception") != std::string::npos)
            return true;
    return false;
}
} // namespace

// A primitive (`throw 5;`) is rejected, not compiled into a crash.
BEZEL_TEST(codegen_cg20, throw_primitive_rejected_739) {
    JitResult r = runAot(
        "module M; class C public static method Main(): int "
        "  throw 5; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(rejectsNonThrowable(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A string value is likewise not throwable.
BEZEL_TEST(codegen_cg20, throw_string_rejected_739) {
    JitResult r = runAot(
        "module M; class C public static method Main(): int "
        "  throw \"boom\"; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(rejectsNonThrowable(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A record is a value type, not a class instance → not throwable.
BEZEL_TEST(codegen_cg20, throw_record_rejected_739) {
    JitResult r = runAot(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  throw new Point(1, 2); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(rejectsNonThrowable(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// An enum value is not throwable.
BEZEL_TEST(codegen_cg20, throw_enum_rejected_739) {
    JitResult r = runAot(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  throw Color.Red; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(rejectsNonThrowable(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// CONTROL: throwing a class instance (user exception) still compiles and runs;
// the caught arm returns 42. Bezel permits throwing any class (§31.1), so a
// non-`Exception`-derived class is accepted.
BEZEL_TEST(codegen_cg20, throw_user_class_still_accepted_739) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppErr end AppErr; "
        "class C public static method Main(): int "
        "  try throw new AppErr(); catch AppErr => return 42; end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// CONTROL: throwing a built-in exception type still compiles and runs.
BEZEL_TEST(codegen_cg20, throw_builtin_exception_still_accepted_739) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  try throw new ArgumentException(\"bad\"); "
        "  catch ArgumentException => return 7; end; "
        "  return 0; "
        "end Main; end C;"), 7);
}
