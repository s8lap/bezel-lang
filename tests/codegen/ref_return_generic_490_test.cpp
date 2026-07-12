// Codegen — constraint member returning a reference type stored into an
// inferred-type local in a generic method body (issue #490).
//
// §14.2: a type parameter T constrained `where T: ISome` lets `x.Member`
// dispatch through the itable/vtable of the concrete type at call sites.
// The UNSPECIALIZED template body must compile to valid (if never-executed)
// IR even when the property's return type is a reference type (string, class)
// — so that the inferred-type local `var n := x.Name` allocates a pointer
// slot, not an i32 slot, and subsequent access `n.Length` (or any method on
// `n`) produces well-formed IR.
//
// Root cause (before fix): in the unspecialized template body T is erased to
// i32. `x.Name` (an interface property returning string) passed the non-ptr
// guard in lowerVirtualPropertyGet and returned nullptr (same guard as #484
// for method calls). The nullptr propagated to lowerLocalDecl, which picked
// i32 as the slot type for `n` (no explicit annotation, no value type). Then
// `n.Length` loaded i32 from the i32 slot and attempted a string property
// call with an i32 argument — malformed IR → LLVM assertion / bezelc crash.
//
// Fix: lowerVirtualPropertyGet, after the record-devirtualization attempt
// fails for a non-ptr receiver in a generic context, returns
// llvm::UndefValue::get(valTy) instead of nullptr. This gives `lowerLocalDecl`
// a correctly-typed (ptr for string) value, so the slot type is right and the
// local is usable in the specialized body where the dispatch is real.
//
// Tests derive directly from §14.2:
//   (a) repro: constraint property (string) stored in inferred local, .Length
//       used — expects 5 ("hello").
//   (b) constraint METHOD returning string stored in inferred local, .Length
//       used — same shape, via method dispatch.
//   (c) constraint property returning a class reference stored in inferred
//       local, then used (class method called on it).
//   (d) annotated-type form `var n: string := x.Name` still works (control —
//       was never broken).
//   (e) constraint member returning int used in arithmetic (control: already
//       worked, must not regress).

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

// ── (a) repro: constraint property returning string, stored in inferred local,
//        .Length used — §14.2  ────────────────────────────────────────────────

// §14.2: `GetN<T>(x: T) where T: IName` reads `x.Name` (string) into an
// inferred `var n := x.Name`, then returns `n.Length`.
// Before the fix: bezelc crashed with malformed IR (i32 slot for n, then
// string-method call with i32 argument).
// Expected: 5 (length of "hello").
BEZEL_TEST(ref_return_generic_490, constraint_property_string_inferred_local_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IName  property Name: string; end IName; "
        "class P : IName "
        "  public _n: string; "
        "  public constructor(n: string) _n := n; end constructor; "
        "  public property Name: string => _n; "
        "  destructor end destructor; "
        "end P; "
        "static class G "
        "  public static method GetN<T>(x: T): int where T: IName "
        "    var n := x.Name; "
        "    return n.Length; "
        "  end GetN; "
        "end G; "
        "class C public static method Main(): int "
        "  using var p := new P(\"hello\"); "
        "  return G.GetN(p); "
        "end Main; end C;"), 5);
}

// ── (b) constraint METHOD returning string, stored in inferred local ──────────

// §14.2: a constraint-bound METHOD (not property) returning string stored in
// an inferred `var n := x.GetName()`, then `n.Length` used — exercises the
// same i32-slot issue via the method-dispatch return path.
// Expected: 3 (length of "hi!").
BEZEL_TEST(ref_return_generic_490, constraint_method_string_inferred_local_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IGreet  method GetName(): string; end IGreet; "
        "class Greeter : IGreet "
        "  public _name: string; "
        "  public constructor(n: string) _name := n; end constructor; "
        "  public method GetName(): string => _name; "
        "  destructor end destructor; "
        "end Greeter; "
        "static class G "
        "  public static method Run<T>(x: T): int where T: IGreet "
        "    var n := x.GetName(); "
        "    return n.Length; "
        "  end Run; "
        "end G; "
        "class C public static method Main(): int "
        "  using var g := new Greeter(\"hi!\"); "
        "  return G.Run(g); "
        "end Main; end C;"), 3);
}

// ── (c) constraint property returning a class reference, used via class method─

// §14.2: constraint property returns a class type (not string) — the inferred
// local holds a class ptr.  `n.Size` (an int property on the class) is called.
// Exercises the same slot-type path with a class reference return.
// Expected: 7.
BEZEL_TEST(ref_return_generic_490, constraint_property_class_ref_inferred_local_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Inner "
        "  public _sz: int; "
        "  public constructor(sz: int) _sz := sz; end constructor; "
        "  public property Size: int => _sz; "
        "  destructor end destructor; "
        "end Inner; "
        "interface IHasInner  property GetInner: Inner; end IHasInner; "
        "class Outer : IHasInner "
        "  public _inner: Inner; "
        "  public constructor(sz: int) _inner := new Inner(sz); end constructor; "
        "  public property GetInner: Inner => _inner; "
        "  destructor _inner.Dispose(); end destructor; "
        "end Outer; "
        "static class G "
        "  public static method GetSize<T>(x: T): int where T: IHasInner "
        "    var n := x.GetInner; "
        "    return n.Size; "
        "  end GetSize; "
        "end G; "
        "class C public static method Main(): int "
        "  using var o := new Outer(7); "
        "  return G.GetSize(o); "
        "end Main; end C;"), 7);
}

// ── (d) annotated form `var n: string := x.Name` — control, must still work ──

// §14.2: same scenario as (a), but with an explicit type annotation on the
// local. The explicit type gives `lowerLocalDecl` `declaredTy = ptr` even when
// `v = nullptr`, so the slot has always been correctly typed.  Verifies the
// workaround remains correct after the fix.
BEZEL_TEST(ref_return_generic_490, annotated_string_local_still_works_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IName  property Name: string; end IName; "
        "class P : IName "
        "  public _n: string; "
        "  public constructor(n: string) _n := n; end constructor; "
        "  public property Name: string => _n; "
        "  destructor end destructor; "
        "end P; "
        "static class G "
        "  public static method GetN<T>(x: T): int where T: IName "
        "    var n: string := x.Name; "
        "    return n.Length; "
        "  end GetN; "
        "end G; "
        "class C public static method Main(): int "
        "  using var p := new P(\"world\"); "
        "  return G.GetN(p); "
        "end Main; end C;"), 5);
}

// ── (e) constraint property returning int used in arithmetic — control ─────────

// §14.2: a constraint property returning int stored in an inferred local, then
// used in arithmetic. This case worked before (#486 fix) and must not regress.
// The slot type for an int property result is i32 — same as the T-erased
// fallback, so no type mismatch occurred — but include it for completeness.
BEZEL_TEST(ref_return_generic_490, constraint_property_int_inferred_local_control_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface ICount  property Count: int; end ICount; "
        "class Counter : ICount "
        "  public _c: int := 0; "
        "  public constructor(c: int) _c := c; end constructor; "
        "  public property Count: int => _c; "
        "  destructor end destructor; "
        "end Counter; "
        "static class G "
        "  public static method Double<T>(x: T): int where T: ICount "
        "    var n := x.Count; "
        "    return n + n; "
        "  end Double; "
        "end G; "
        "class C public static method Main(): int "
        "  using var c := new Counter(21); "
        "  return G.Double(c); "
        "end Main; end C;"), 42);
}
