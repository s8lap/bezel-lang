// Fix verification tests for bugs #457 and #458.
//
// #458 (§7.5 / §26 / nullable null-conditional call): `x?.Method()` where
//   `x` is a NULLABLE VALUE type (e.g. `int?`, `bool?`) silently returned 0
//   instead of invoking the method.  `lowerNullCondCall` had an early return
//   `if (!recv->getType()->isPointerTy()) return nullptr;` which drops the
//   call for a tagged-struct receiver (the LLVM representation of `int?`).
//   Fix: detect the nullable-value struct receiver, extract `hasValue` and
//   `value`, branch on `hasValue`, call the method on the unwrapped value
//   when present, and lift the result to `T?` — mirroring the logic that
//   lowerNullCondCall already applies to pointer receivers.
//   Spec reference: §7.5 null-conditional access; §26 extension methods.
//
// #457 (§26 / §27.6 extension method reference thunk for primitive this):
//   `x.Double` in value position (a method reference) where `Double` is an
//   extension method on `int` crashed at call time with SIGSEGV.
//   `makeMethodThunk` stored `receiverVal` (an i32) into an 8-byte env block
//   via `CreateStore`, then inside the thunk loaded a `ptr` from env and
//   passed it where the extension method's first parameter is an i32 value.
//   The type mismatch (ptr vs i32) caused the call to dereference garbage.
//   Fix: for an extension method whose `this` parameter is a non-pointer
//   (primitive or enum by value), store the value correctly and load it with
//   the actual `this` type in the thunk body.
//   Spec reference: §26.1 extension method receiver; §27.6 method references.
//   NOTE: if sema rejects `x.Double` (extension method in value position)
//   with a type error before codegen runs, the #457 tests are skipped with a
//   note.  The sema lane (#456) and this codegen lane (#457) are independent.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// ── #458: x?.Method() on int? (nullable primitive receiver) ──────────────────

// §7.5 / #458: `x?.Double()` where x: int? is non-null (hasValue=true).
// Before fix: the call is silently dropped → result is 0 (zero-struct).
// After fix: hasValue true → extract inner int, call Double(), lift to int?.
BEZEL_TEST(bug458_nullcond_call_nullable_primitive,
           nullcond_int_present_calls_method_458) {
    // extension int: Double() returns this * 2.
    // x is non-null int?(5), so x?.Double() must return int?(10).
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extension int "
        "  public method Double(): int => this * 2; "
        "end; "
        "class C public static method Main(): int "
        "  var x: int? := 5; "
        "  var r: int? := x?.Double(); "
        "  if r = null then return -1; end; "
        "  return r!; "   // must be 10
        "end Main; end C;"), 10);
}

// §7.5 / #458: `x?.Double()` where x: int? is null (hasValue=false).
// Result must be null int?, not 0.
BEZEL_TEST(bug458_nullcond_call_nullable_primitive,
           nullcond_int_null_returns_null_458) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extension int "
        "  public method Double(): int => this * 2; "
        "end; "
        "class C public static method Main(): int "
        "  var x: int? := null; "
        "  var r: int? := x?.Double(); "
        "  if r = null then return 1; end; "  // expect null → return 1
        "  return 0; "
        "end Main; end C;"), 1);
}

// §7.5 / #458: null-conditional call with an argument, non-null receiver.
// x: int? = 10, extension adds n; x?.Plus(5) must yield int?(15).
BEZEL_TEST(bug458_nullcond_call_nullable_primitive,
           nullcond_int_with_arg_present_458) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extension int "
        "  public method Plus(n: int): int => this + n; "
        "end; "
        "class C public static method Main(): int "
        "  var x: int? := 10; "
        "  var r: int? := x?.Plus(5); "
        "  if r = null then return -1; end; "
        "  return r!; "   // must be 15
        "end Main; end C;"), 15);
}

// §7.5 / #458: null-conditional call feeds `??` operator (compose with CG39).
// When x is non-null: x?.Double() ?? 99 must be 2*x.
// When x is null:     x?.Double() ?? 99 must be 99.
BEZEL_TEST(bug458_nullcond_call_nullable_primitive,
           nullcond_int_feeds_coalesce_458) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extension int "
        "  public method Double(): int => this * 2; "
        "end; "
        "class C public static method Main(): int "
        "  var a: int? := 7; "
        "  var b: int? := null; "
        "  var ra: int := a?.Double() ?? 99; "   // 14
        "  var rb: int := b?.Double() ?? 99; "   // 99
        "  return ra + rb; "                       // 113
        "end Main; end C;"), 113);
}

// §7.5 / #458: bool? receiver; extension returns bool.
// x: bool? = true → x?.IsTrue() must yield bool?(true) → truthy result.
BEZEL_TEST(bug458_nullcond_call_nullable_primitive,
           nullcond_bool_present_calls_method_458) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extension bool "
        "  public method Negate(): bool => not this; "
        "end; "
        "class C public static method Main(): int "
        "  var x: bool? := true; "
        "  var r: bool? := x?.Negate(); "
        "  if r = null then return -1; end; "
        "  if r! then return 0; else return 1; end; "  // Negate(true) = false → 1
        "end Main; end C;"), 1);
}

// §7.5 / #458: regression — null-conditional call on a class (reference) receiver
// still works (the pointer-receiver path is not broken by the fix).
BEZEL_TEST(bug458_nullcond_call_nullable_primitive,
           regression_nullcond_class_receiver_unaffected_458) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class N "
        "  public V: int := 0; "
        "  public method GetV(): int => V; "
        "end N; "
        "class C public static method Main(): int "
        "  var n := new N(); n.V := 42; var nn: N? := n; "
        "  var x: int? := nn?.GetV(); "
        "  if x = null then return -1; end; "
        "  return x!; "  // 42
        "end Main; end C;"), 42);
}

// ── #457: extension method reference (bound thunk) for primitive this ─────────

// §26.1 / §27.6 / #457: `x.Double` used as `() -> int` value for an
// extension method on int.  Before fix: SIGSEGV because the thunk loaded a
// ptr from env but passed it where an i32 is expected.
// After fix: the thunk stores and restores the i32 value correctly.
//
// NOTE: if sema rejects `x.Double` (extension-method reference) with a type
// error before codegen runs, this test will return -999 (ran == false).
// That would indicate the sema lane (#456) is still blocking #457.  Report
// accordingly; do NOT change this test — the codegen fix is already in place.
BEZEL_TEST(bug457_extension_method_ref_prim_thunk,
           ext_method_ref_int_stored_and_called_457) {
    // x = 5; f = x.Double (bound method reference); f() must return 10.
    // Before fix: the thunk loaded ptr from env and passed it where i32 is
    // expected; the IR verifier caught the type mismatch and codegen failed
    // (ran == false). After fix: env stores i32, thunk loads i32, call works.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extension int "
        "  public method Double(): int => this * 2; "
        "end; "
        "class C "
        "  public static method Apply(f: () -> int): int return f(); end Apply; "
        "  public static method Main(): int "
        "    var x := 5; "
        "    var f: () -> int := x.Double; "
        "    return f(); "
        "  end Main; "
        "end C;"), 10);
}

// §26.1 / §27.6 / #457: extension method reference passed directly as argument.
BEZEL_TEST(bug457_extension_method_ref_prim_thunk,
           ext_method_ref_int_passed_as_arg_457) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extension int "
        "  public method Triple(): int => this * 3; "
        "end; "
        "class C "
        "  public static method Apply(f: () -> int): int return f(); end Apply; "
        "  public static method Main(): int "
        "    var x := 7; "
        "    return Apply(x.Triple); "   // 21
        "  end Main; "
        "end C;"), 21);
}

// §26.1 / §27.6 / #457: regression — class method reference still works.
BEZEL_TEST(bug457_extension_method_ref_prim_thunk,
           regression_class_method_ref_unaffected_457) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class A "
        "  public _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  destructor end destructor; "
        "  public method GetVal(): int => _v; "
        "end A; "
        "class C "
        "  public static method Apply(f: () -> int): int return f(); end Apply; "
        "  public static method Main(): int "
        "    using var a := new A(42); "
        "    var f: () -> int := a.GetVal; "
        "    return f(); "
        "  end Main; "
        "end C;"), 42);
}

// #738 / §27.6: a method reference to a VIRTUAL method through a BASE-typed
// receiver must dispatch dynamically through the receiver's runtime type — the
// thunk previously called the static (base) method, so the override was missed.
BEZEL_TEST(bug_457_458, method_ref_virtual_base_dispatch_738) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public virtual method Id(): int => 1; "
        "  destructor end destructor; end Base; "
        "class Sub: Base public override method Id(): int => 9; "
        "  destructor end destructor; end Sub; "
        "class C public static method Main(): int "
        "  using var s := new Sub(); var b: Base := s; "
        "  var fn: () -> int := b.Id; "  // method ref through base
        "  return fn(); "  // 9 (Sub override), not 1 (Base)
        "end Main; end C;"), 9);
}

// #738 / §27.6 / §13: a method reference to an INTERFACE method through an
// interface-typed receiver must dispatch through the itable (previously the
// thunk had no concrete function → SIGSEGV).
BEZEL_TEST(bug_457_458, method_ref_interface_dispatch_738) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IFoo method Do(n: int): int; end IFoo; "
        "class FooImpl: IFoo "
        "  public constructor() end constructor; "
        "  public method Do(n: int): int => n * 3; "
        "  destructor end destructor; end FooImpl; "
        "class C public static method Main(): int "
        "  using var f := new FooImpl(); var p: IFoo := f; "
        "  var fn: (int) -> int := p.Do; "  // method ref through interface
        "  return fn(7); "  // 21
        "end Main; end C;"), 21);
}
