// Regression tests for issue #4963 — §5.1 "Nullable conversions" (`S → T → T?`)
// at a call argument in the CLOSURE-CALL and GENERIC-SPECIALIZATION paths: an
// `int` argument bound to a `long?` formal of a closure, or to a `T?` formal
// of a generic method instantiated at `T = long`.
//
// #4922 fixed the widen-then-wrap in buildOrderedArgs /
// fillOmittedParamDefaults. The sibling wrap sites wrapped a plain argument
// into the tagged `{i8, T}` struct only when its LLVM type was EXACTLY the
// payload type, so a narrower numeric argument escaped unwrapped:
//   * closure call `f(7)` against `(long?) -> long`: IR verification failure
//     (`call i64 %f(ptr %env, i32 7)` against a `{ i8, i64 }` parameter);
//   * `Id<long>(5)` against `Id<T>(x: T?)`: the specialization was built with
//     an `i32` parameter the body read as `{i8, i64}` — bezelc SIGSEGV (rc
//     139, no diagnostic) in lowerCoalesce → nullablePrimHasValue.
//
// spec §5.1 "Implicit conversions" (int → long / double, uint → long) and
// "Nullable conversions" (`S` converts to `T?` via `S → T → T?`), §14.1
// (monomorphization substitutes the supplied type argument), §22.6 stage 3,
// v0.307.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

void expectPrints4963(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// per §5.1 (v0.307), fix #4963 — the issue's repro 1 verbatim: an `int`
// literal at a closure's `long?` formal (pre-fix: IR verification failure).
BEZEL_TEST(nullable_arg_widen_wrap_4963, issue_repro1_closure_int_at_long_nullable) {
    expectPrints4963(
        "module M; "
        "class P "
        "  public static method Main(): int "
        "    var f := lambda(x: long?): long => x ?? -3; "
        "    Console.WriteLine(f(7)); return 0; "
        "  end Main; "
        "end P;",
        "7\n");
}

// per §5.1 / §14.1 (v0.307), fix #4963 — the issue's repro 2 verbatim: an
// `int` literal at a generic `T?` formal with `T = long` (pre-fix: bezelc
// SIGSEGV).
BEZEL_TEST(nullable_arg_widen_wrap_4963, issue_repro2_generic_int_at_T_nullable_long) {
    expectPrints4963(
        "module M; "
        "class P "
        "  public static method Id<T>(x: T?): T where T: value "
        "    return x ?? default(T); end Id; "
        "  public static method Main(): int "
        "    Console.WriteLine(Id<long>(5)); return 0; "
        "  end Main; "
        "end P;",
        "5\n");
}

// per §5.1 (v0.307), fix #4963 — the closure-call arm with a negative `int`
// local (sign-extended), a `uint` above 2^31 (zero-extended, so the widen
// must honour the source's signedness), an explicit `null`, and `int` →
// `double?`.
BEZEL_TEST(nullable_arg_widen_wrap_4963, closure_signedness_null_and_double) {
    expectPrints4963(
        "module M; "
        "class P "
        "  public static method Main(): int "
        "    var f := lambda(x: long?): long => x ?? -3; "
        "    var g := lambda(x: double?): double => x ?? 0.5; "
        "    var n: int := -9; var u: uint := 4000000000; "
        "    Console.WriteLine(f(n)); Console.WriteLine(f(u)); "
        "    Console.WriteLine(f(null)); Console.WriteLine(g(3)); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "-9\n4000000000\n-3\n3\n");
}

// per §5.1 / §14.1 (v0.307), fix #4963 — the generic-specialization arm:
// a block-bodied and an expression-bodied generic method (the two
// emitInlineGenericMethod bindings), and a generic CLASS's method whose
// `T?` formal resolves through the receiver's `Box<long>` instantiation.
// `int` locals, negative and `uint` > 2^31, plus `int` → `double?`.
BEZEL_TEST(nullable_arg_widen_wrap_4963, generic_block_expr_and_class_method) {
    expectPrints4963(
        "module M; "
        "class Box<T> where T: value "
        "  public constructor() end constructor; "
        "  public method Or(x: T?, d: T): T return x ?? d; end Or; "
        "end Box; "
        "class P "
        "  public static method Id<T>(x: T?): T where T: value "
        "    return x ?? default(T); end Id; "
        "  public static method IdE<T>(x: T?): T where T: value "
        "    => x ?? default(T); "
        "  public static method Main(): int "
        "    var n: int := -9; var u: uint := 4000000000; "
        "    Console.WriteLine(Id<long>(n)); Console.WriteLine(Id<long>(u)); "
        "    Console.WriteLine(Id<double>(2)); "
        "    Console.WriteLine(IdE<long>(n)); Console.WriteLine(IdE<long>(u)); "
        "    using var b := new Box<long>(); "
        "    Console.WriteLine(b.Or(n, 1)); Console.WriteLine(b.Or(u, 1)); "
        "    Console.WriteLine(b.Or(null, 1)); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "-9\n4000000000\n2\n-9\n4000000000\n-9\n4000000000\n1\n");
}

// per §5.1 / §14.1 (v0.307), fix #4963 — 50 calls in a loop through both
// arms; the running sum pins every widened-and-wrapped value.
BEZEL_TEST(nullable_arg_widen_wrap_4963, closure_and_generic_loop_50) {
    expectPrints4963(
        "module M; "
        "class P "
        "  public static method Id<T>(x: T?): T where T: value "
        "    return x ?? default(T); end Id; "
        "  public static method Main(): int "
        "    var f := lambda(x: long?): long => x ?? -3; "
        "    var sum: long := 0; "
        "    for var i in 0..<50 do "
        "      sum := sum + f(i) + Id<long>(i); "
        "    end; "
        "    Console.WriteLine(sum); return 0; "
        "  end Main; "
        "end P;",
        "2450\n");
}

// per §5.1 / §14.1 (v0.307), fix #4963 — the generic-specialization lift
// inside a generic CALLER whose own type parameter shares the callee's name
// `T` and is bound to the SAME type (`Go<long>` calling `Id<long>(n)`): the
// explicit type argument confirms the name-keyed binding, so the lift runs.
BEZEL_TEST(nullable_arg_widen_wrap_4963, same_name_caller_T_confirmed_by_explicit_arg) {
    expectPrints4963(
        "module M; "
        "class U "
        "  public static method Id<T>(x: T?): T where T: value "
        "    return x ?? default(T); end Id; "
        "  public static method Go<T>(v: T): string "
        "    var n: int := -9; var u: uint := 4000000000; "
        "    return $\"{Id<long>(n)} {Id<long>(u)} {Id<long>(5)}\"; "
        "  end Go; "
        "end U; "
        "class P "
        "  public static method Main(): int "
        "    Console.WriteLine(U.Go<long>(1)); return 0; "
        "  end Main; "
        "end P;",
        "-9 4000000000 5\n");
}

// per §5.1 / §14.1 / §14.5 (v0.307), fix #4963 follow-up. The caller's
// same-named `T` is bound to a DIFFERENT type (`Go<int?>` calling
// `Id<long>(3)`). A method's type parameters are scoped to that method
// (§14.1/§14.5; #4813 is fixed — applyMethodGenericSubst binds the callee's
// `T` from this call site), so the #4963 widen-then-wrap lift must apply at
// T := long and the program prints `3`. A stale guard premised on #4813 still
// being open skipped the lift, and bezelc crashed (SIGSEGV) instead; the row
// used to accept a compile failure, so it now asserts the program RAN.
BEZEL_TEST(nullable_arg_widen_wrap_4963, same_name_caller_T_collision_never_wrong_value_4813) {
    auto r = runAot(
        "module M; "
        "class U "
        "  public static method Id<T>(x: T?): T where T: value "
        "    return x ?? default(T); end Id; "
        "  public static method Go<T>(v: T): string => $\"{Id<long>(3)}\"; "
        "end U; "
        "class P "
        "  public static method Main(): int "
        "    Console.WriteLine(U.Go<int?>(2)); return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}
