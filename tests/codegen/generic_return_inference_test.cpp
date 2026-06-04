// Codegen / end-to-end — return-target generic type-argument inference
// (issue #391).
//
// Sema solves a generic method's return-only type parameter from the call's
// target type: `var w: Widget := Factory.Create();` infers T = Widget in
// `Create<T>(): T` (see tests/sema/generic_return_inference_test.cpp for the
// sema-level assertions and the spec basis — §14.1/§14.2 + §A.7, with the
// "target type" concept from §15.4 / §17.7).
//
// These tests run the full pipeline (parse → sema → lower → JIT) and assert
// the facts that hold end-to-end:
//
//   * the no-target form is rejected at sema, so the program does not run; and
//   * the typed-local and return forms type-check, lower, AND construct the
//     inferred T — the inferred type's constructor runs exactly once per call.
//
// CODEGEN (issue #391, completed). lowerCall now consumes the return-target-
// inferred type argument: sema re-types the call to the concrete return type
// and records it on the call's exprType; lowerCall unifies the method's
// *declared* return TypeRef against that concrete type to recover the unbound
// method type parameter and primes `genericSubst_`/`genericSubstSemaTy_` before
// emitting the body, so `new T()` constructs the real type instead of falling
// to the i32 placeholder (LlvmBackend_Calls.cpp::unifyMethodReturnTypeArg).
// Before this codegen change the typed-local program below SEGFAULTED at run
// time (`new T()` constructed nothing); the constructor-observing asserts are
// the load-bearing regression check that the right T is now chosen.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
const char* kFactory =
    "module M; "
    "class Widget "
    "  public static _n: int := 0; "
    "  public constructor() _n := _n + 1; end constructor; "
    "  destructor end destructor; "
    "end Widget; "
    "static class Factory "
    "  public static method Create<T>(): T where T: class, new() "
    "    return new T(); "
    "  end Create; "
    "end Factory; ";
}  // namespace

// §14.1 / §15.4: a typed-local target solves the return-only T, and codegen
// constructs the inferred `Widget` — its constructor runs exactly once, so
// `Widget._n == 1`. (LOAD-BEARING: before the lowerCall fix this program
// SEGFAULTED, because `new T()` lowered `T` to the i32 placeholder and
// constructed nothing.)
BEZEL_TEST(generic_return_inference_391, typed_local_constructs_inferred_type) {
    std::string prog = std::string(kFactory) +
        "class C public static method Main(): int "
        "  using var w: Widget := Factory.Create(); "
        "  return Widget._n; "
        "end Main; end C;";
    auto r = runJit(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // Widget's constructor ran exactly once
}

// §14.1 / §15.4: two return-target-inferred calls each construct a fresh
// `Widget`, so the static counter reaches 2. Guards against the inferred T
// being bound once and leaking (or never being re-derived per call).
BEZEL_TEST(generic_return_inference_391, two_calls_construct_twice) {
    std::string prog = std::string(kFactory) +
        "class C public static method Main(): int "
        "  using var a: Widget := Factory.Create(); "
        "  using var b: Widget := Factory.Create(); "
        "  return Widget._n; "
        "end Main; end C;";
    auto r = runJit(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);  // two distinct Widgets constructed
}

// §15.4 / §17.7: the `return Factory.Create();` form supplies the enclosing
// declared return type (`Widget`) as the target; codegen constructs the
// inferred `Widget` in the called helper, so its constructor runs once.
BEZEL_TEST(generic_return_inference_391, return_target_constructs_inferred_type) {
    std::string prog = std::string(kFactory) +
        "class C "
        "  public static method GetWidget(): owned Widget "
        "    return Factory.Create(); "
        "  end GetWidget; "
        "  public static method Main(): int "
        "    using var w := GetWidget(); "
        "    return Widget._n; "
        "  end Main; "
        "end C;";
    auto r = runJit(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // GetWidget's inferred Widget constructed
}

// §15.4 / §17.7 / §A.7: a `var` without annotation gives the call no target.
// The return-only T cannot be solved, so sema rejects the program — it never
// runs. This is the load-bearing end-to-end check for the no-target diagnostic.
BEZEL_TEST(generic_return_inference_391, no_target_is_rejected) {
    std::string prog = std::string(kFactory) +
        "class C public static method Main(): int "
        "  var w := Factory.Create(); "
        "  return 0; "
        "end Main; end C;";
    auto r = runJit(prog);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}

// guard: an argument-inferred generic method (T bound by `proto: T`) still
// works end-to-end and is independent of any target — the return-target path
// does not interfere with the existing #361 argument-inference codegen.
BEZEL_TEST(generic_return_inference_391, argument_inferred_unaffected) {
    std::string prog =
        "module M; "
        "class Widget "
        "  public static _n: int := 0; "
        "  public constructor() _n := _n + 1; end constructor; "
        "  destructor end destructor; "
        "end Widget; "
        "static class Factory "
        "  public static method Clone<T>(proto: T): T where T: class, new() "
        "    return new T(); "
        "  end Clone; "
        "end Factory; "
        "class C public static method Main(): int "
        "  using var p := new Widget(); "
        "  using var c := Factory.Clone(p); "
        "  return Widget._n; "  // p's ctor + new T()'s ctor (argument-inferred)
        "end Main; end C;";
    auto r = runJit(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}
