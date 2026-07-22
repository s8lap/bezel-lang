// Codegen — generic-method monomorphization for non-int value-type arguments
// (issues #504 and #505).
//
// #505 (IR-verification failure — "ret void / i32"):
//   A static generic method `GetV<T>(obj:T, getter:(T)->int):int => getter(obj)`
//   called with T = a CLASS emitted malformed IR: the lambda closure `(x)=>x.v`
//   was compiled with the unresolved placeholder type for `x` (i32, the generic
//   fallback for T) because `buildOrderedArgs` set `expectedFunctionType_` to
//   `(T)->int` BEFORE the method-level generic substitution (T=class ptr) was
//   established.  Inside `lowerLambda` the `lowerTypeRef(T)` call returned i32,
//   the `expected != i32()` guard rejected the upgrade, and `x` stayed i32.
//   Then `x.v` tried to load a vtable-field from an i32 receiver, which returns
//   null → the closure body emitted `ret void` in an i32-returning function →
//   IR verifier rejected: "Function return type does not match operand type of
//   return inst! ret void / i32".  Fix: infer the method-level generic
//   substitution from non-lambda arguments BEFORE `buildOrderedArgs` runs, so
//   `lowerTypeRef(T)` returns the concrete LLVM type (ptr for a class) and the
//   lambda param is correctly typed.
//
// #504 (silent wrong result — worse than a crash):
//   `Identity<T>(v:T):T` called with T = a RECORD returns an all-zero record
//   (T=int works by luck). Root cause: sema's `typeCall` used `substituteMemberType`
//   to resolve the call's return type, but that helper only substitutes
//   CLASS-level generic params (from `ms->parent->genericParams`). For a METHOD-
//   level generic (`static method Identity<T>` on a non-generic class `Wrapper`),
//   `Wrapper` has no generic params, so the return type stayed as the bare `T`
//   TypeRef. Then `Identity(r).x` tried `ctx_.exprSymbol(Identity(r).x)` — which
//   resolved `.x` on the bare-T TypeRef, found nothing, and returned null.
//   `fieldAddr` got null → `lowerExpr(Identity(r).x)` = null → `ret i32 0`.
//   Fix: in `typeCall`, after `substituteMemberType`, additionally substitute
//   method-level generic params that are bound by call arguments.
//
// Spec references:
//   §14.1 / §14.2 — generic methods are monomorphized at each call site; the
//   return type is the substituted type (not the bare type parameter).
//   §9.1 / §9.4 — records are value types; `Identity(r).x` must round-trip the
//   field value through the generic boundary.
//   §27.3 — lambdas passed to generic methods must compile with the concrete
//   param type (not the unresolved generic placeholder).

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

// ── #505: generic method + closure param typed as class ──────────────────────

// §14.2 / §27.3 / #505: `GetV<T>(obj:T, getter:(T)->int):int => getter(obj)`
// called with T=A (class). The closure `(x) => x.v` must be compiled with `x`
// typed as `ptr` (A's LLVM type), not `i32` (the generic placeholder). If
// broken: IR verification failure "ret void / i32" (compiler exits non-zero).
// This is the exact repro from the bug report.
BEZEL_TEST(codegen_generic_mono_505, closure_param_typed_as_class_14_2_27_3) {
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

// §14.2 / §27.3 / #505: two class fields, second field (v2=3). The closure
// reads v2. Verifies that the correct field is accessed (layout is correct, not
// just nonzero).
BEZEL_TEST(codegen_generic_mono_505, closure_reads_second_field_14_2_27_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B "
        "  public v1: int; "
        "  public v2: int; "
        "  public constructor() v1 := 2; v2 := 3; end constructor; "
        "  destructor end destructor; "
        "end B; "
        "class Util "
        "  public static method GetV<T>(obj: T, getter: (T) -> int): int "
        "    => getter(obj); "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new B(); "
        "    return Util.GetV(b, lambda(x) => x.v2); "
        "  end Main; "
        "end C;"), 3);
}

// §14.2 / §27.3 / #505: generic method returning a class T (not int).
// The method is block-bodied. The closure arg is not used; verifies that
// a generic method returning a class T works when T=class (no IR mismatch).
BEZEL_TEST(codegen_generic_mono_505, generic_method_returns_class_T_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class W "
        "  public n: int; "
        "  public constructor(x: int) n := x; end constructor; "
        "  destructor end destructor; "
        "end W; "
        "class Util "
        "  public static method Pick<T>(a: T, b: T, flag: int): T "
        "    if flag <> 0 then return a; else return b; end; "
        "  end Pick; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    using var w1 := new W(10); "
        "    using var w2 := new W(20); "
        "    using var picked := Util.Pick(w1, w2, 1); "
        "    return picked.n; "
        "  end Main; "
        "end C;"), 10);
}

// §14.2 / §27.3 / #505: CONTROL — T=int still works (regression guard).
// The closure `(x) => x * 2` with T=int must still return the correct value.
BEZEL_TEST(codegen_generic_mono_505, control_T_int_closure_still_works_14_2_27_3) {
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

// ── #504: generic Identity with record T preserves fields ────────────────────

// §14.1 / §9.1 / §9.4 / #504: `Identity<T>(v:T):T` with T=record returns the
// correct record value. `Identity(r).x` must round-trip field x=77 through the
// generic boundary. If broken: returns 0 (all-zero record or no call emitted).
// This is the exact repro from the bug report.
BEZEL_TEST(codegen_generic_mono_504, identity_record_chained_field_14_1_9_1) {
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

// §14.1 / §9.1 / #504: two fields; Identity returns the record; chained access
// selects the SECOND field. Verifies field-slot correctness, not just nonzero.
BEZEL_TEST(codegen_generic_mono_504, identity_record_chained_second_field_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record TwoFlds(a: int, b: int); "
        "class Wrapper "
        "  public static method Identity<T>(v: T): T "
        "    return v; "
        "  end Identity; "
        "end Wrapper; "
        "class C "
        "  public static method Main(): int "
        "    var r := new TwoFlds(3, 99); "
        "    return Wrapper.Identity(r).b; "
        "  end Main; "
        "end C;"), 99);
}

// §14.1 / §9.1 / #504: Identity result stored in a typed local before field
// access — the workaround that already worked before the fix. Must still work.
BEZEL_TEST(codegen_generic_mono_504, identity_record_via_typed_local_14_1) {
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
        "    var r := new OneFld(55); "
        "    var s: OneFld := Wrapper.Identity(r); "
        "    return s.x; "
        "  end Main; "
        "end C;"), 55);
}

// §14.2 / #504: CONTROL — Identity<T> with T=int still works (regression guard).
BEZEL_TEST(codegen_generic_mono_504, control_identity_T_int_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Wrapper "
        "  public static method Identity<T>(v: T): T "
        "    return v; "
        "  end Identity; "
        "end Wrapper; "
        "class C "
        "  public static method Main(): int "
        "    return Wrapper.Identity(42); "
        "  end Main; "
        "end C;"), 42);
}

// §14.2 / #504: CONTROL — the existing generic-constraint-dispatch tests
// (generic_constraint_dispatch_484_test) and spec-cache tests must still pass.
// Verified by the overall test suite; this test exercises that a RECORD T
// alongside a CLASS T in the same program works (no specialization collision).
BEZEL_TEST(codegen_generic_mono_504, identity_record_and_int_coexist_14_1) {
    // §14.1: record T and int T specializations coexist. Formula stays <= 255.
    // rx=3, ry=4, ri=10 → rx*7 + ry*5 + ri = 21+20+10 = 51.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(x: int, y: int); "
        "class Wrapper "
        "  public static method Identity<T>(v: T): T "
        "    return v; "
        "  end Identity; "
        "end Wrapper; "
        "class C "
        "  public static method Main(): int "
        "    var p := new Pt(3, 4); "
        "    var rx := Wrapper.Identity(p).x; "
        "    var ry := Wrapper.Identity(p).y; "
        "    var ri := Wrapper.Identity(10); "
        "    return rx * 7 + ry * 5 + ri; "
        "  end Main; "
        "end C;"), 51);
}
