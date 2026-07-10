// Regression tests for issue #1261 (§27.5 / §14.1): a lambda that captures
// and returns a generic-`T` parameter inside a GENERIC-CLASS instance method,
// called at a concrete class-typed instantiation, must return the captured
// object so member access on the result resolves.
//
// §14.1 (per-instantiation evaluation): "For a generic type or member, the
// property is evaluated at each instantiation, after substituting the type
// arguments." A method `Lift(x: T): () -> T` on `Wrapper<T>` therefore returns
// `() -> Payload` (not the bare `() -> T`) when called on a `Wrapper<Payload>`,
// so `var result := f(); result._v` types `result` as `Payload` and the field
// resolves.
//
// §27.5 (closures): "A lambda may reference variables from its enclosing
// scope. Captured variables are heap-allocated in a closure object."
//
// Root cause (NOT a codegen capture/truncation bug, despite the issue title):
// the captured pointer was already correct at runtime (a `Wrapper<Payload>`
// specialization sized the env cell and thunk return as `ptr`). The actual
// defect was in SEMA return-type substitution: `substituteMemberType` bailed
// out for any non-`Named` return type, and `substituteReturnType` did not
// recurse into `Kind::Function`, so a `() -> T` return was left with an
// unresolved inner `T`. The call result was then typed as the bare type
// parameter, and member access on it (`result._v`, `result.GetV()`) failed to
// lower — Main returned 0. Fix: thread the substitution through Function
// (funcParams/funcReturn) and InlineArray (arrayElement) return types.
//
// Controls:
//   - value-type T (int): `Wrapper<int>.Lift(42)` capturing+returning — must
//     keep working (the i32 placeholder happened to equal int before the fix).
//   - `(T) -> T` (function-typed return with a generic param): exercises
//     funcParam + funcReturn substitution.
//   - multi-param `Pair<A, B>` returning `() -> B`: the non-first parameter
//     must substitute.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// The exact #1261 repro: a generic-class method whose `() -> T` return captures
// a class-typed argument; the result's field is read. Before the fix this
// returned 0 (the field access failed to lower against the unresolved T).
std::string classTypedProgram(int payloadVal, int expected) {
    (void)expected;
    return std::string(
        "module M; "
        "class Payload "
        "  public _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  destructor end destructor; "
        "end Payload; "
        "class Wrapper<T> "
        "  public constructor() end constructor; "
        "  public method Lift(x: T): () -> T "
        "    return () => x; "
        "  end Lift; "
        "  destructor end destructor; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  using var p := new Payload(") +
        std::to_string(payloadVal) + "); "
        "  using var w := new Wrapper<Payload>(); "
        "  var f := w.Lift(p); "
        "  var result := f(); "
        "  return result._v; "
        "end Main; end C;";
}

} // namespace

// §14.1 / §27.5 — the headline #1261 case: `Wrapper<Payload>.Lift(p)` captures
// a class-typed `T`, the closure returns it, and `result._v` reads the field.
// Before the fix: 0 (unresolved inner T → member access did not lower).
BEZEL_TEST(codegen_1261, lambda_generic_capture_class_t_spec_27_5_14_1) {
    BEZEL_EXPECT_EQ(run(classTypedProgram(42, 42)), 42);
}

// §14.1 / §27.5 — a different field value guards against a hard-coded result.
BEZEL_TEST(codegen_1261, lambda_generic_capture_class_t_alt_value_spec_27_5_14_1) {
    BEZEL_EXPECT_EQ(run(classTypedProgram(99, 99)), 99);
}

// §14.1 control — value-type T (int): `Wrapper<int>.Lift(42)` capturing+returning
// must keep working (i32 placeholder == int by luck before the fix; the
// substituted return type is now `() -> int`).
BEZEL_TEST(codegen_1261, lambda_generic_capture_value_t_control_spec_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Wrapper<T> "
        "  public constructor() end constructor; "
        "  public method Lift(x: T): () -> T "
        "    return () => x; "
        "  end Lift; "
        "  destructor end destructor; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  using var w := new Wrapper<int>(); "
        "  var f := w.Lift(42); "
        "  return f(); "
        "end Main; end C;"), 42);
}

// §14.1 / §27.5 — the closure result is also usable through a METHOD call
// (not just a field), confirming the call result is typed `Payload`, not the
// bare `T`. Before the fix this returned 0 too.
BEZEL_TEST(codegen_1261, lambda_generic_capture_method_call_spec_27_5_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Payload "
        "  public _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method GetV(): int return _v; end GetV; "
        "  destructor end destructor; "
        "end Payload; "
        "class Wrapper<T> "
        "  public constructor() end constructor; "
        "  public method Lift(x: T): () -> T "
        "    return () => x; "
        "  end Lift; "
        "  destructor end destructor; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  using var p := new Payload(42); "
        "  using var w := new Wrapper<Payload>(); "
        "  var f := w.Lift(p); "
        "  var result := f(); "
        "  return result.GetV(); "
        "end Main; end C;"), 42);
}

// §14.1 / §27 — a `(T) -> T` function-typed return (generic param in BOTH a
// funcParam and funcReturn) instantiated with a class type. Exercises the
// funcParam substitution the fix added to substituteReturnType.
BEZEL_TEST(codegen_1261, lambda_generic_funcparam_funcret_spec_27_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Payload "
        "  public _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  destructor end destructor; "
        "end Payload; "
        "class Wrapper<T> "
        "  public constructor() end constructor; "
        "  public method Bind(x: T): (T) -> T "
        "    return (y: T) => y; "
        "  end Bind; "
        "  destructor end destructor; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  using var p := new Payload(42); "
        "  using var w := new Wrapper<Payload>(); "
        "  var f := w.Bind(p); "
        "  var result := f(p); "
        "  return result._v; "
        "end Main; end C;"), 42);
}

// §14.1 — multi-param generic `Pair<A, B>` returning `() -> B`: the NON-first
// type parameter (B) must substitute to the concrete class type.
BEZEL_TEST(codegen_1261, lambda_generic_second_param_spec_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Payload "
        "  public _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  destructor end destructor; "
        "end Payload; "
        "class Pair<A, B> "
        "  public constructor() end constructor; "
        "  public method SecondGetter(b: B): () -> B "
        "    return () => b; "
        "  end SecondGetter; "
        "  destructor end destructor; "
        "end Pair; "
        "class C public static method Main(): int "
        "  using var p := new Payload(77); "
        "  using var pr := new Pair<int, Payload>(); "
        "  var f := pr.SecondGetter(p); "
        "  var result := f(); "
        "  return result._v; "
        "end Main; end C;"), 77);
}
