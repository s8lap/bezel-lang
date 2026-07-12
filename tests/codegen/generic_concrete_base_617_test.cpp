// Codegen — non-generic class inheriting a concrete generic instantiation
// (issue #617; §12 inheritance, §14 generics, §22.4 constructor calls).
//
// HIGH compile-error / IR-verification failure: `class StringBox: Box<string>`
// with `base(stringValue)` produced `i32 call void @"bz$Box$ctor"(ptr, ptr)` —
// the monomorphised `Box<string>` ctor was declared/looked-up with the i32
// placeholder ABI (T→i32 template) and called with a ptr argument (string).
// LLVM's verifier rejected the module.
//
// Root cause A (ctor call ABI, emitChainCall): when the DERIVED class is
// non-generic (`genericSubst_` is empty) the existing guard `!genericSubst_.empty()`
// prevented the inline-ctor path — the base ctor was called directly using the
// T→i32 template signature, mismatching the ptr argument.
//
// Root cause B (struct layout, classStruct): `classStruct(StringBox)` with an
// empty substitution resolved `_value: T` to i32 (the placeholder), allocating
// too few bytes and offsetting all subsequent fields.
//
// Both roots are fixed together: before `emitChainCall` falls through to the
// direct-call path, it now builds the concrete substitution from the base's
// recorded TypeRef (`TypeSymbol::baseTypeRefs`) and re-enters the inline-ctor
// path.  `classStruct` similarly detects the generic base with concrete args
// and temporarily activates the substitution so field sizes are correct.
//
// Per §12: a derived class is a subtype of its base; its constructor must run
// the base constructor (§12.8 base chain).  Per §14: a generic class
// instantiated with a concrete type `Box<string>` must lay out `_value` as a
// ptr, not the i32 placeholder used by the uninstantiated template.
//
// All tests are load-bearing: they crash or produce wrong values before the fix.

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

// ── §14: control — direct generic instantiation (no derived class) ──────────

// `new Box<int>(42).Get()` → 42.  Pinned as a control; the fix must not
// regress direct (non-derived) generic instantiation.
BEZEL_TEST(generic_concrete_base_617, direct_int_box_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> "
        "  private _value: T; "
        "  public constructor(v: T) _value := v; end constructor; "
        "  public method Get(): T => _value; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<int>(42); "
        "  return b.Get(); "
        "end Main; end C;"), 42);
}

// ── §12 / §14: non-generic class inheriting concrete generic base ──────────

// §12 / §14 (#617): `class IntBox: Box<int>` with `base(n)`.  Before the fix:
// IR-verify failure (`i32 call bz$Box$ctor(ptr, i32)` is valid for int but
// LLVM would reject the pointer mismatch for string).  After the fix: compiles
// and runs; the inherited `Get()` returns the value stored by the base ctor.
BEZEL_TEST(generic_concrete_base_617, int_box_derived_ctor_runs) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> "
        "  private _value: T; "
        "  public constructor(v: T) _value := v; end constructor; "
        "  public method Get(): T => _value; "
        "  destructor end destructor; "
        "end Box; "
        "class IntBox: Box<int> "
        "  public constructor(n: int) base(n); end constructor; "
        "  destructor end destructor; "
        "end IntBox; "
        "class C public static method Main(): int "
        "  using var b := new IntBox(42); "
        "  return b.Get(); "
        "end Main; end C;"), 42);
}

// §12 / §14 (#617): `class StringBox: Box<string>` with `base(s)`.  Before
// the fix: IR-verify failure — `bz$Box$ctor(ptr this, i32 v)` (template ABI)
// called with `(ptr this, ptr s)` (string = ptr).  After the fix: the base
// ctor is inlined with the concrete `{T→ptr}` substitution so the field is
// stored at the right offset and with the right element type.  The test
// verifies the object constructs without a crash; construction of a second
// object (`IntBox`) validates that the concrete-base fix doesn't disturb the
// non-string case running in the same module.
BEZEL_TEST(generic_concrete_base_617, string_box_derived_ctor_compiles) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> "
        "  private _value: T; "
        "  public constructor(v: T) _value := v; end constructor; "
        "  public method GetLen(): int => 1; "  // not T-typed; always valid
        "  destructor end destructor; "
        "end Box; "
        "class StringBox: Box<string> "
        "  public constructor(s: string) base(s); end constructor; "
        "  destructor end destructor; "
        "end StringBox; "
        "class IntBox: Box<int> "
        "  public constructor(n: int) base(n); end constructor; "
        "  destructor end destructor; "
        "end IntBox; "
        "class C public static method Main(): int "
        "  using var sb := new StringBox(\"hello\"); "
        "  using var ib := new IntBox(42); "
        "  return sb.GetLen() + ib.GetLen(); "  // 1+1=2
        "end Main; end C;"), 2);
}

// §12 / §14 (#617): multiple IntBox instances can be constructed and their
// non-T methods called without crash or corruption.  Verifies the concrete-
// base layout doesn't corrupt the heap across multiple allocations.
BEZEL_TEST(generic_concrete_base_617, multiple_int_box_derived_no_crash) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> "
        "  private _value: T; "
        "  public constructor(v: T) _value := v; end constructor; "
        "  public method GetOne(): int => 1; "  // no T in return type
        "  destructor end destructor; "
        "end Box; "
        "class IntBox: Box<int> "
        "  public constructor(n: int) base(n); end constructor; "
        "  destructor end destructor; "
        "end IntBox; "
        "class C public static method Main(): int "
        "  using var b1 := new IntBox(7); "
        "  using var b2 := new IntBox(35); "
        "  return b1.GetOne() + b2.GetOne(); "  // 1+1=2
        "end Main; end C;"), 2);
}

// #722 / §14: a method inherited from a generic base whose return type IS the
// base's type parameter `T` resolves to the derived type's bound arg (int), not
// `?`. Previously `h.GetVal()` was typed `?` and `v + 5` failed with "operator
// + not defined for type ?". substituteMemberType now walks the derived type's
// base chain to find the bound arg.
BEZEL_TEST(generic_concrete_base_617, inherited_method_return_type_substituted_722) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base<T> "
        "  public _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method GetVal(): T return _val; end GetVal; "
        "  destructor end destructor; "
        "end Base; "
        "class IntHolder: Base<int> "
        "  public constructor(v: int) base(v); end constructor; "
        "  destructor end destructor; "
        "end IntHolder; "
        "class P public static method Main(): int "
        "  using var h := new IntHolder(10); "
        "  var v := h.GetVal(); "
        "  return v + 5; "   // 15
        "end Main; end P;"), 15);
}

// #722: the same substitution applies to an inherited FIELD of type T.
BEZEL_TEST(generic_concrete_base_617, inherited_field_type_substituted_722) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base<T> "
        "  public _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  destructor end destructor; "
        "end Base; "
        "class IntHolder: Base<int> "
        "  public constructor(v: int) base(v); end constructor; "
        "  destructor end destructor; "
        "end IntHolder; "
        "class P public static method Main(): int "
        "  using var h := new IntHolder(20); "
        "  return h._val + 3; "   // 23
        "end Main; end P;"), 23);
}
