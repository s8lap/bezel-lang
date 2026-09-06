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

#include "support/aot_result_asserts.hpp"   // fix #3895: failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {
const char* kFactory =
    "module M; "
    "class Widget "
    "  public static _n: int := 0; "
    "  public constructor() _n := _n + 1; end constructor; "
    "  destructor end destructor; "
    "end Widget; "
    "static class Factory "
    "  public static method Create<T>(): owned T where T: class, new() "
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
    auto r = runAot(prog);
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
    auto r = runAot(prog);
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
    auto r = runAot(prog);
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
    auto r = runAot(prog);
    // fix #3895 (issue #3892): distinguishes a genuine §15.4/§17.7 rejection
    // from a runtime crash/signal, which a bare `hasErrors()`/`!ran` pair
    // cannot.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
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
        // §28.3 v0.194 (#1801): a fresh `new T()` returned while the source set is
        // non-empty ({proto}) must transfer via `owned T` — the callee-side bare-
        // `T` admission rejects a fresh owned value through a non-`owned` return.
        // T is still argument-inferred from `proto` (the return spelling does not
        // affect the §14.1 inference this test exercises).
        "static class Factory "
        "  public static method Clone<T>(proto: T): owned T where T: class, new() "
        "    return new T(); "
        "  end Clone; "
        "end Factory; "
        "class C public static method Main(): int "
        "  using var p := new Widget(); "
        "  using var c := Factory.Clone(p); "
        "  return Widget._n; "  // p's ctor + new T()'s ctor (argument-inferred)
        "end Main; end C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// fix #747 R43 / §14.1: EXPLICIT call-site type arguments fully determine a
// generic method's type parameters, so NO target type is required — the
// "cannot infer ... target type required" diagnostic must not fire. Previously
// `var q := C.Swap<int,int>(p);` was rejected (it appeared to need a target)
// even though the type arguments were spelled out, while the equivalent typed
// local compiled.

// A return-position-only T solved by explicit type args, bound to an UNTYPED
// local — must compile and run. v0.201 (#2048): the empty-set bare-T signature
// is spelled `owned T` per the declaration-site static-return rule; at the
// value instantiation `int` the marker is erased (§28.1), so the call yields a
// plain `int` and the local binds without an ownership obligation.
BEZEL_TEST(generic_return_inference_391, explicit_type_args_no_target_needed_747) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Make<T>(): owned T return default(T); end Make; "
        "  public static method Main(): int "
        "    var x := C.Make<int>(); "  // explicit <int> → no target needed
        "    return x; "  // default(int) = 0
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Explicit type args on a method whose params are bound through a generic-typed
// parameter and whose return swaps them — `Swap<int,int>(p)` into an untyped
// local. Returns the swapped first field.
BEZEL_TEST(generic_return_inference_391, explicit_type_args_generic_param_swap_747) {
    auto r = runAot(
        "module M; "
        "record Pair<A, B>(a: A, b: B); "
        "class C "
        "  public static method Swap<A, B>(p: Pair<A, B>): Pair<B, A> "
        "    return new Pair<B, A>(p.b, p.a); "
        "  end Swap; "
        "  public static method Main(): int "
        "    var p := new Pair<int, int>(3, 7); "
        "    var q := C.Swap<int, int>(p); "  // untyped local, explicit args
        "    return q.a; "  // swapped → 7
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// CONTROL: WITHOUT explicit type args, a return-only T into an untyped local is
// still rejected (the diagnostic must remain for genuine inference-needs-target).
BEZEL_TEST(generic_return_inference_391, no_explicit_args_still_rejected_747) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Make<T>(): T return default(T); end Make; "
        "  public static method Main(): int "
        "    var x := C.Make(); "  // no type args, no target → rejected
        "    return 0; "
        "  end Main; "
        "end C;");
    // fix #3895 (issue #3892): distinguishes a genuine §15.4/§17.7 rejection
    // from a runtime crash/signal, which a bare `hasErrors()`/`!ran` pair
    // cannot.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
