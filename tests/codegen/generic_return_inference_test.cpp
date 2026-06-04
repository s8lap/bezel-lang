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
// the facts that hold end-to-end after the sema change:
//
//   * the no-target form is rejected at sema, so the program does not run; and
//   * the typed-local and return forms type-check and produce a runnable image
//     (the sema re-typing does not break lowering).
//
// CODEGEN FOLLOW-UP (NOT YET LANDED). Observing the *constructor* of the
// inferred T run — the strongest end-to-end proof that the right T is chosen —
// is blocked on a codegen change that this milestone intentionally does not
// make (the codegen lanes own LlvmBackend_*). Codegen monomorphizes a generic
// method call by re-deriving its type-parameter substitution from the argument
// types in lowerCall (LlvmBackend_Calls.cpp); a return-only parameter is bound
// by no argument, so `genericSubst_` never receives T and `new T()` in the body
// constructs nothing (the bare `T` lowers to the i32 fallback). To pick up a
// return-target-inferred argument, lowerCall must derive the unbound parameter
// from the call's target/result type (e.g. `ctx_.exprType(call)`) and prime
// `genericSubst_` before emitting the body. The asserts below are therefore
// scoped to what sema alone makes observable; the disabled-by-comment line in
// `typed_local_runs` records the value the codegen follow-up must produce.

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

// §14.1 / §15.4: a typed-local target solves the return-only T and the program
// type-checks and lowers to a runnable image (sema's re-typing of the call to
// the concrete `Widget` does not break codegen).
BEZEL_TEST(generic_return_inference_391, typed_local_compiles_and_runs) {
    std::string prog = std::string(kFactory) +
        "class C public static method Main(): int "
        "  var w: Widget := Factory.Create(); "
        "  using var ww := w; "
        "  return 0; "
        "end Main; end C;";
    auto r = runJit(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    // CODEGEN FOLLOW-UP: once lowerCall derives T from the target, the inferred
    // Widget constructor must run exactly once. Re-targeting this program at
    // `return Widget._n;` should then yield 1 (it currently yields 0 because
    // `new T()` constructs nothing — see the file header).
}

// §15.4 / §17.7: the return form supplies the enclosing return type as target;
// the program type-checks and lowers.
BEZEL_TEST(generic_return_inference_391, return_target_compiles_and_runs) {
    std::string prog = std::string(kFactory) +
        "class C "
        "  public static method GetWidget(): Widget "
        "    return Factory.Create(); "
        "  end GetWidget; "
        "  public static method Main(): int "
        "    using var w := GetWidget(); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runJit(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
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
