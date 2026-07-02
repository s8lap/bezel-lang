// Codegen — virtual/interface dispatch on a collection element inside a
// generic METHOD with a constraint (`where T: Base` / `where T: IFoo`).
// Issue #1246 / §12 / §14.2.
//
// ROOT CAUSE: `unifyMethodReturnTypeArg` (LlvmBackend_Calls.cpp) handled
// two shapes for binding a method type-parameter:
//   (a) bare T vs concrete B     → first branch (single-part, no decl) → bind
//   (b) Box<T> vs Box<Widget>    → second branch (both have declaration) → recurse
//
// Builtin generic types like OwningArray<T>, Array<T>, LinkedList<T> have a
// null declaration (they are compiler-known, not user-defined). Shape (b)'s
// guard `declRet->declaration` was false for them, so neither branch fired:
// the function returned early without recursing into the generic args and T
// remained unbound.  With T unbound, the force-specialisation check at the
// call site (emitMethodCall ~line 12343) also did not trigger, so the method
// body ran as the i32-erased compiled template.  Inside that template, `x: T`
// is i32 (not a pointer), so `emitMethodCall`'s non-pointer guard returned
// nullptr for `x.V()`, yielding 0 instead of the override's return value.
//
// FIX (§12/§14.2, issue #1246): a third recursive case in
// `unifyMethodReturnTypeArg`: when the declared type is a builtin generic
// type (no declaration, has generic args) and the actual type carries the
// same builtin name, recurse pairwise into their generic args — so
// `OwningArray<T>` vs `OwningArray<B>` binds T=B, triggering correct
// per-instantiation inlining and vtable/itable dispatch on elements.
//
// Tests derive from §12 (dynamic dispatch) and §14.2 (generic method
// constraints) of the Bezel spec:
//
//   (a) for-in loop element — OwningArray<T> where T: Base class constraint
//   (b) for-in loop element — OwningArray<T> where T: IFoo (interface constraint)
//   (c) indexer element     — arr[0].V() inside generic method-level T
//   (d) Array<T>            — borrowing array variant (not OwningArray)
//   (e) LinkedList<T>       — linked-list variant
//   (f) CONTROL: direct param `x: T` dispatch still works (was already correct)
//   (g) CONTROL: class-level GContainer<T> dispatch still works
//   (h) CONTROL: non-virtual (non-override) method under constraint → no regression
//   (i) CONTROL: non-generic virtual call still correct

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── (a) for-in / OwningArray<T> where T: Base class constraint ────────────────

// §14.2 / §12 (#1246): inside a generic method `Sum<T>(arr: OwningArray<T>)
// where T: B`, iterating with `for var x in arr` and calling `x.V()` must
// dispatch to the most-derived OVERRIDE (D.V = 1), not B.V (= 0). Before the
// fix, T was never bound from OwningArray<T> vs OwningArray<B>, so the method
// ran as the i32-erased template and `x.V()` returned 0 (no dispatch).
BEZEL_TEST(virtual_dispatch_collection_elem_1246,
           forin_owned_array_class_constraint_spec_12_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B public virtual method V(): int => 0; "
        "  destructor end destructor; end B; "
        "class D : B "
        "  public constructor() end constructor; "
        "  public override method V(): int => 1; "
        "  destructor end destructor; "
        "end D; "
        "static class Util "
        "  public static method Sum<T>(arr: OwningArray<T>): int where T: B "
        "    var s := 0; for var x in arr do s := s + x.V(); end; return s; "
        "  end Sum; "
        "end Util; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<B>(); "
        "  a.Add(new D()); a.Add(new D()); "
        "  return Util.Sum(a); "   // D.V()=1 twice → 2
        "end Main; end C;"), 2);
}

// ── (b) for-in / OwningArray<T> where T: IFoo interface constraint ─────────────

// §14.2 / §13 (#1246): interface constraint `where T: IFoo`. The element is
// loaded from the OwningArray and dispatched through the itable. Before the
// fix: W() returned 0 (no dispatch). After: reaches A.W() = 7, sum = 14.
BEZEL_TEST(virtual_dispatch_collection_elem_1246,
           forin_owned_array_iface_constraint_spec_12_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IFoo method W(): int; end IFoo; "
        "class A : IFoo "
        "  public constructor() end constructor; "
        "  public method W(): int => 7; "
        "  destructor end destructor; "
        "end A; "
        "static class G "
        "  public static method Run<T>(arr: OwningArray<T>): int where T: IFoo "
        "    var s := 0; for var x in arr do s := s + x.W(); end; return s; "
        "  end Run; "
        "end G; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<A>(); "
        "  a.Add(new A()); a.Add(new A()); "
        "  return G.Run(a); "   // A.W()=7 twice → 14
        "end Main; end C;"), 14);
}

// ── (c) indexer element dispatch inside generic method-level T ───────────────

// §14.2 / §12 (#1246): element accessed via indexer `arr[0]` (not for-in),
// then calling a virtual method on it. Before the fix: V() dispatched as i32
// stub → 0. After: reaches D.V() = 3.
BEZEL_TEST(virtual_dispatch_collection_elem_1246,
           indexer_elem_class_constraint_spec_12_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B public virtual method V(): int => 0; "
        "  destructor end destructor; end B; "
        "class D : B "
        "  public constructor() end constructor; "
        "  public override method V(): int => 3; "
        "  destructor end destructor; "
        "end D; "
        "static class G "
        "  public static method Get<T>(arr: OwningArray<T>): int where T: B "
        "    var e := arr[0]; return e.V(); "
        "  end Get; "
        "end G; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<B>(); a.Add(new D()); "
        "  return G.Get(a); "   // D.V() = 3
        "end Main; end C;"), 3);
}

// ── (d) borrowing Array<T> variant ───────────────────────────────────────────

// §14.2 / §12 (#1246): same bug exists for the borrowing `Array<T>`.
// `Run<T>(arr: Array<T>)` with T=B, elements are D instances. Before the
// fix: 0. After: D.V()=5 twice → 10.
BEZEL_TEST(virtual_dispatch_collection_elem_1246,
           forin_array_class_constraint_spec_12_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B public virtual method V(): int => 0; "
        "  destructor end destructor; end B; "
        "class D : B "
        "  public constructor() end constructor; "
        "  public override method V(): int => 5; "
        "  destructor end destructor; "
        "end D; "
        "static class G "
        "  public static method Run<T>(arr: Array<T>): int where T: B "
        "    var s := 0; for var x in arr do s := s + x.V(); end; return s; "
        "  end Run; "
        "end G; "
        "class C public static method Main(): int "
        "  using var a := new Array<B>(); "
        "  a.Add(new D()); a.Add(new D()); "
        "  return G.Run(a); "   // D.V()=5 twice → 10
        "end Main; end C;"), 10);
}

// ── (e) LinkedList<T> variant ────────────────────────────────────────────────

// §14.2 / §12 (#1246): same unification fix for LinkedList<T>. D.V()=4 twice
// → 8. Before the fix: 0.
BEZEL_TEST(virtual_dispatch_collection_elem_1246,
           forin_linked_list_class_constraint_spec_12_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B public virtual method V(): int => 0; "
        "  destructor end destructor; end B; "
        "class D : B "
        "  public constructor() end constructor; "
        "  public override method V(): int => 4; "
        "  destructor end destructor; "
        "end D; "
        "static class G "
        "  public static method Run<T>(ll: LinkedList<T>): int where T: B "
        "    var s := 0; for var x in ll do s := s + x.V(); end; return s; "
        "  end Run; "
        "end G; "
        "class C public static method Main(): int "
        "  using var ll := new LinkedList<B>(); "
        "  ll.AddLast(new D()); ll.AddLast(new D()); "
        "  return G.Run(ll); "   // D.V()=4 twice → 8
        "end Main; end C;"), 8);
}

// ── (f) CONTROL: direct-param `x: T` dispatch still correct ──────────────────

// §14.2 / §12: a generic method `GetRank<T>(x: T)` with a direct method
// param of type T (not wrapped in a collection). This was already correct
// before #1246 (T→i32 vs ptr type mismatch triggers inline). Must remain 99.
BEZEL_TEST(virtual_dispatch_collection_elem_1246,
           control_direct_param_dispatch_still_correct_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public constructor() end constructor; "
        "  public virtual method Rank(): int => 1; "
        "  destructor end destructor; "
        "end Base; "
        "class Sub : Base "
        "  public constructor() end constructor; "
        "  public override method Rank(): int => 99; "
        "  destructor end destructor; "
        "end Sub; "
        "static class G "
        "  public static method GetRank<T>(x: T): int where T: Base "
        "    return x.Rank(); "
        "  end GetRank; "
        "end G; "
        "class C public static method Main(): int "
        "  using var s := new Sub(); return G.GetRank(s); "
        "end Main; end C;"), 99);
}

// ── (g) CONTROL: class-level generic constraint dispatch still correct ─────────

// §12 / §14.1: a generic METHOD on a STATIC class with a SECOND type param U
// (distinct from T) that is also constrained. After the #1246 fix, type-param
// inference for one param must not disturb the other. Specifically, binding
// T=OwningArray<B> from `arr: OwningArray<T>` must not clobber U=B in a
// two-param generic method. Both calls must still dispatch correctly.
BEZEL_TEST(virtual_dispatch_collection_elem_1246,
           control_two_type_params_no_interference_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B public virtual method V(): int => 0; destructor end destructor; end B; "
        "class D : B "
        "  public constructor() end constructor; "
        "  public override method V(): int => 2; "
        "  destructor end destructor; "
        "end D; "
        "static class G "
        "  public static method Run<T, U>(arr: OwningArray<T>, extra: U): int "
        "    where T: B where U: B "
        "    var s := extra.V(); "
        "    for var x in arr do s := s + x.V(); end; "
        "    return s; "
        "  end Run; "
        "end G; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<B>(); "
        "  a.Add(new D()); a.Add(new D()); "
        "  using var e := new D(); "
        "  return G.Run(a, e); "  // T=B, U=D: D.V()=2 from extra + D.V()=2 twice from arr → 6
        "end Main; end C;"), 6);
}

// ── (h) CONTROL: non-virtual (concrete) method under constraint → no regression

// §14.2: calling a NON-VIRTUAL (non-override) method on a collection element
// under a method-level constraint. The base class method is called directly
// (no vtable slot). Before and after the fix: result is 42.
BEZEL_TEST(virtual_dispatch_collection_elem_1246,
           control_nonvirtual_method_on_elem_no_regression_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Helper "
        "  public _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Val(): int => _v; "
        "  destructor end destructor; "
        "end Helper; "
        "static class G "
        "  public static method Get<T>(arr: OwningArray<T>): int where T: Helper "
        "    var e := arr[0]; return e.Val(); "
        "  end Get; "
        "end G; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<Helper>(); "
        "  a.Add(new Helper(42)); "
        "  return G.Get(a); "
        "end Main; end C;"), 42);
}

// ── (i) CONTROL: non-generic virtual call (no type params) still correct ──────

// §12: a plain non-generic method with a virtual call. The #1246 fix touches
// `unifyMethodReturnTypeArg` which is only invoked for generic method params;
// a non-generic virtual call must be unaffected.
BEZEL_TEST(virtual_dispatch_collection_elem_1246,
           control_nongeneric_virtual_call_still_correct_spec_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B2 public virtual method R(): int => 10; destructor end destructor; end B2; "
        "class D2 : B2 "
        "  public constructor() end constructor; "
        "  public override method R(): int => 55; "
        "  destructor end destructor; "
        "end D2; "
        "class C public static method Main(): int "
        "  using var d := new D2(); "
        "  return d.R(); "
        "end Main; end C;"), 55);
}
