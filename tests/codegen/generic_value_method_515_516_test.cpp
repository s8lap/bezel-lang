// Codegen — generic-method monomorphization bugs with value/reference type args
// (issues #515 and #516).
//
// #516 (IR-verification failure — "Function return type does not match operand
// type of return inst! ret void / ptr"):
//   A static generic method `Apply<U>(x:int, fn:(int)->U):U` called with U=string
//   via `return Helper.Apply<string>(42, (x) => x.ToString())` from a string-returning
//   method produced malformed IR. U appears ONLY in the lambda RETURN position (not
//   as any non-lambda parameter's type). The pre-pass added in #505 infers the
//   method-level generic substitution from non-lambda arguments, but the non-lambda
//   arg here (`x`) has declared type `int` (not `U`), so the pre-pass leaves U
//   unbound. When `lowerLambda` then tries to resolve `expectedFunctionType_->funcReturn`
//   (which is `U`), `lowerTypeRef(U)` returns the i32 placeholder. The lambda body
//   `(x) => x.ToString()` returns a string ptr, but the closure function was built
//   with fnTy `(ptr, i32) -> i32` (the placeholder), so the actual `ret ptr` in the
//   body mismatches the declared `i32` return → IR verification failure.
//
//   Note: U can ALSO be resolved from the call's *result* type (via
//   unifyMethodReturnTypeArg at the main-substitution stage), but that runs AFTER
//   the lambda is already compiled during the pre-pass. Fix: in the pre-pass,
//   ALSO seed genericSubst_ from the call's explicit typeArgs (U is given as string
//   in `Apply<string>(...)`), so lowerTypeRef(U) = ptr before lowerLambda runs.
//
// #515 (SIGSEGV at runtime):
//   A static generic method `AsString<U>(v:U):string where U:notnull` with body
//   `return v.ToString()` called with U=int (i.e. `AsString<int>(42)`) caused a
//   SIGSEGV. Inside `emitGenericSpecialization`, the body `v.ToString()` was lowered
//   with `v` having sema type `U` (an unresolved Named TypeRef, no declaration).
//   The sema `typeCall` for `v.ToString()` did not set a MethodSymbol (because
//   `U:notnull` provides no type-constraint surface), so `calleeSym` was null in
//   `lowerCall`. The `!ms` branch for `ToString` only fires for Object/interface
//   receivers; since the receiver's sema type is an unresolved generic param (not
//   Object or interface), neither guard matched. The call returned nullptr →
//   `emitReturn(specFn, nullptr)` emitted `ret ptr null` for the string return →
//   caller received a null string pointer → SIGSEGV on first string dereference.
//   Fix: in the `!ms` branch of `lowerCall`, detect "receiver is a type-param with
//   an active value-type substitution (i32/i64/i1/f64/f32)" and route `ToString()`
//   directly to `emitPrimitiveToString` on the lowered receiver value.
//
// Spec references:
//   §14.1 / §14.2 — generic methods are monomorphized at each call site; all
//     type-parameter references resolve to concrete types.
//   §27.3 — lambdas passed to generic methods must compile with the concrete
//     param/return types, not the unresolved generic placeholder.
//   §8.5 — ToString() is available on every type; for primitives it returns the
//     canonical decimal/boolean string representation.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
bezel::test::JitResult jit(const std::string& prog) {
    return runAot(prog);
}

// Helper for #516: U appears ONLY in the lambda return position.
// Apply<U>(x:int, fn:(int)->U):U — U is NOT in any non-lambda param.
// NOTE: the call must be placed in a context that provides U's return type
// (a method with declared return type, or a typed local `var x: T :=`),
// because sema cannot infer U solely from the call's args when U only
// appears in the return position (§14.1, §A.7).
const char* kApply =
    "module M; "
    "class Helper "
    "  public static method Apply<U>(x: int, fn: (int) -> U): U where U: notnull "
    "    => fn(x); "
    "end Helper; ";

// Helper for #515: U is the value param type; body calls v.ToString().
const char* kAsString =
    "module M; "
    "class Helper "
    "  public static method AsString<U>(v: U): string where U: notnull "
    "    return v.ToString(); "
    "  end AsString; "
    "end Helper; ";

}  // namespace

// ── #516: U in lambda return position only ────────────────────────────────────

// §14.1 / §27.3 / #516: Apply<string>(42, (x)=>x.ToString()) — U=string is
// ONLY in the lambda return type. Before fix: IR verification failure
// "ret void / ptr" (or similar) because the lambda's closure function was
// compiled with fnTy `(ptr, i32) -> i32` (placeholder for U) before the
// explicit U=string substitution was established, and the `ret ptr` inside
// mismatched the declared i32 return. After fix: the lambda's closure function
// is compiled with the correct return type ptr.
// The call is placed inside a string-returning method to provide the return
// context that sema needs to resolve U (per §14.1, §A.7).
BEZEL_TEST(codegen_generic_value_method_516, apply_string_lambda_return_no_irfail_14_1_27_3) {
    BEZEL_EXPECT_EQ(run(
        std::string(kApply) +
        "class C "
        "  public static method GetStr(): string "
        "    return Helper.Apply<string>(42, lambda(x) => x.ToString()); "
        "  end GetStr; "
        "  public static method Main(): int "
        "    var s := C.GetStr(); "
        "    if s = \"42\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// §14.1 / §27.3 / #516: Apply<int> — U=int; the lambda doubles the value.
// Uses a typed local to provide the return context for sema.
BEZEL_TEST(codegen_generic_value_method_516, apply_int_control_still_works_14_1_27_3) {
    BEZEL_EXPECT_EQ(run(
        std::string(kApply) +
        "class C "
        "  public static method Main(): int "
        "    var v: int := Helper.Apply<int>(5, lambda(x) => x * 2); "
        "    return v; "
        "  end Main; "
        "end C;"), 10);
}

// §14.1 / §27.3 / #516: Apply<bool> — U=bool in lambda return; (x) => x > 0.
BEZEL_TEST(codegen_generic_value_method_516, apply_bool_lambda_return_14_1_27_3) {
    BEZEL_EXPECT_EQ(run(
        std::string(kApply) +
        "class C "
        "  public static method GetBool(): bool "
        "    return Helper.Apply<bool>(3, lambda(x) => x > 0); "
        "  end GetBool; "
        "  public static method Main(): int "
        "    if C.GetBool() then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// §14.1 / §27.3 / #516: Apply with a REF-type U (class) — must still work as
// it did before (reference types already dispatched correctly via vtable).
// v0.201 (#2048): the fresh-producing factory is spelled `owned Box` per the
// declaration-site static-return rule.
// v0.223 / fix #1820: the §28.3 producer row's callee-side rule requires a
// lambda at a concrete class/interface slot to spell `owned` to transfer —
// `fn: (int) -> U` (plain U) classifies the invocation as a borrow and the
// lambda's `new Box(x)` is rejected as a fresh producer under that
// classification. The remedy is the stdlib §4.14 generic-factory convention
// `fn: (int) -> owned U` — §28.1's per-instantiation erasure keeps value
// substitutions unchanged, so the migration is a one-line declaration
// change with no caller-side fallout. The runtime return path stays the
// same: `GetBox(): owned Box` reads the lambda's owned-mint straight into
// its own transferring return slot.
BEZEL_TEST(codegen_generic_value_method_516, apply_class_ref_type_U_still_works_14_1_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box "
        "  public n: int; "
        "  public constructor(v: int) n := v; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class Helper "
        "  public static method Apply<U>(x: int, fn: (int) -> owned U): owned U where U: notnull "
        "    => fn(x); "
        "end Helper; "
        "class C "
        "  public static method GetBox(): owned Box "
        "    return Helper.Apply<Box>(7, lambda(x) => new Box(x)); "
        "  end GetBox; "
        "  public static method Main(): int "
        "    using var b := C.GetBox(); "
        "    return b.n; "
        "  end Main; "
        "end C;"), 7);
}

// §14.1 / §27.3 / #516: #505 original shape — T in lambda PARAM position (not
// just return). Must still work (regression guard for the original fix).
BEZEL_TEST(codegen_generic_value_method_516, original_505_shape_regression_guard_14_2_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class A "
        "  public v: int; "
        "  public constructor() v := 7; end constructor; "
        "  destructor end destructor; "
        "end A; "
        "class Util "
        "  public static method GetV<T>(obj: T, getter: (T) -> int): int "
        "    => getter(obj); "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new A(); "
        "    return Util.GetV(a, lambda(x) => x.v); "
        "  end Main; "
        "end C;"), 7);
}

// ── #515: value-type-monomorphized U calling ToString() ──────────────────────

// §14.1 / §8.5 / #515: AsString<int>(42) — U=int, body calls v.ToString().
// Before fix: SIGSEGV because `v.ToString()` inside the specialized body
// lowered to nullptr (no route for a generic-param receiver with notnull
// constraint), and the caller received a null string ptr → crash on first
// dereference. After fix: routes to emitPrimitiveToString(int) → "42".
BEZEL_TEST(codegen_generic_value_method_515, asstring_int_no_sigsegv_14_1_8_5) {
    auto r = jit(
        std::string(kAsString) +
        "class C "
        "  public static method Main(): int "
        "    var s := Helper.AsString<int>(42); "
        "    if s = \"42\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §14.1 / §8.5 / #515: AsString<bool>(true) — U=bool, body calls v.ToString().
// Per stdlib §5 / #452, bool.ToString() returns "True"/"False" (capital first letter).
BEZEL_TEST(codegen_generic_value_method_515, asstring_bool_14_1_8_5) {
    auto r = jit(
        std::string(kAsString) +
        "class C "
        "  public static method Main(): int "
        "    var s := Helper.AsString<bool>(true); "
        "    if s = \"True\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §14.1 / §8.5 / #515: AsString<double>(0.0) — U=double; just verify no crash.
BEZEL_TEST(codegen_generic_value_method_515, asstring_double_no_crash_14_1_8_5) {
    auto r = jit(
        std::string(kAsString) +
        "class C "
        "  public static method Main(): int "
        "    var s := Helper.AsString<double>(0.0); "
        "    if s.Length >= 0 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §14.1 / §8.5 / #515: AsString with a CLASS-type U — reference types already
// dispatch correctly via the existing path. Regression guard.
BEZEL_TEST(codegen_generic_value_method_515, asstring_class_ref_type_control_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class MyVal "
        "  public n: int; "
        "  public constructor(v: int) n := v; end constructor; "
        "  public method ToString(): string => \"MyVal:\"; "
        "  destructor end destructor; "
        "end MyVal; "
        "class Helper "
        "  public static method AsString<U>(v: U): string where U: notnull "
        "    return v.ToString(); "
        "  end AsString; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    using var mv := new MyVal(99); "
        "    var s := Helper.AsString<MyVal>(mv); "
        "    if s = \"MyVal:\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// ── Combined #515 + #516 ──────────────────────────────────────────────────────

// §14.1 / §8.5 / #515 + #516 combined: a method that uses a lambda (testing
// #516's explicit-typearg pre-pass) AND calls ToString in the block body
// (testing #515's primitive-dispatch fix).
BEZEL_TEST(codegen_generic_value_method_515_516, combined_lambda_and_tostring_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Helper "
        "  public static method Transform<U>(x: int, fn: (int) -> U): string "
        "      where U: notnull "
        "    var v := fn(x); "
        "    return v.ToString(); "
        "  end Transform; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var s := Helper.Transform<int>(7, lambda(x) => x * 6); "
        "    if s = \"42\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// ── Regression guards for previously-fixed issues ────────────────────────────

// §14.1 / #504 regression guard: Identity<T>(v:T):T with T=record must still
// work (the #516 pre-pass must not affect the inferred path).
BEZEL_TEST(codegen_generic_value_method_515_516, identity_record_regression_guard_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record OneFld(x: int); "
        "class Wrapper "
        "  public static method Identity<T>(v: T): T "
        "    return v; "
        "  end Identity; "
        "end Wrapper; "
        "class C "
        "  public static method Main(): int "
        "    var r := new OneFld(77); "
        "    return Wrapper.Identity(r).x; "
        "  end Main; "
        "end C;"), 77);
}

// §14.1 / #509 regression guard: explicit type args still work.
BEZEL_TEST(codegen_generic_value_method_515_516, explicit_typeargs_509_regression_guard_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Wrapper "
        "  public static method Identity<T>(v: T): T "
        "    return v; "
        "  end Identity; "
        "end Wrapper; "
        "class C "
        "  public static method Main(): int "
        "    var x := Wrapper.Identity<int>(42); "
        "    if x = 42 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// §14.1 / §27.3 / #505 regression guard: (T)->int shape still works.
BEZEL_TEST(codegen_generic_value_method_515_516, original_505_lambda_regression_guard_14_2_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Util "
        "  public static method Apply<T>(v: T, f: (T) -> int): int => f(v); "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    return Util.Apply(5, lambda(x) => x * 2); "
        "  end Main; "
        "end C;"), 10);
}
