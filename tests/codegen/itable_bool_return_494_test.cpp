// Codegen tests for issue #494: a bool-returning interface method called via the
// builtin IEnumerator<T> vtable dispatch, stored in an inferred-var slot, was
// mis-typed as i32 (the fallback default) instead of i1/bool.
//
// Root cause: when the receiver's static type is the builtin IEnumerator<T>
// (no sema TypeDecl, no MethodSymbol for MoveNext), lowerCall had no handler for
// the explicit `e.MoveNext()` call — it returned nullptr, leaving the inferred
// local slot typed as i32. A subsequent `if moved then` emitted `br i32` which
// the LLVM IR verifier rejects ("Branch condition is not i1 type"). The for-loop
// path avoided this by dispatching vtable slot 2 directly (lowerIEnumerableBuiltinFor),
// never going through lowerCall.
//
// Fix (§29.2 / §29.3): in lowerCall's !ms branch, detect MoveNext/Current on a
// builtin IEnumerator<T>/OwnedEnumerator<T> receiver and dispatch through the
// iterator vtable directly (slot 2 → i1 for MoveNext, slot 3 → T for Current),
// matching the vtable layout emitted by emitIteratorMethod. Also type these calls
// in sema (MoveNext → bool, Current → T) so collectSuspendLocals and annotated
// local declarations see the correct type.
//
// Tests:
//  A: exact repro — `var moved := e.MoveNext(); if moved then return 1;` → 1
//  B: 3-item manual MoveNext/Current loop summing values → 6
//  C: annotated `var b: bool := e.MoveNext()` → 1
//  D: non-iterator interface with bool method via interface-typed var → 1
//  E (control): `for var x in iter` still works (no regression)
//  F (control): vtable bool method on a class var (not interface) → 1

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ---- A: Exact repro from issue #494 -------------------------------------------
// per §29.2 / §29.3: `var moved := e.MoveNext()` where e: IEnumerator<int> from
// an iterator method. Before the fix the slot was i32 and `br i32` failed the
// IR verifier; after the fix the slot is i1 and the branch is valid.
BEZEL_TEST(itable_bool_return_494, repro_inferred_var_movenext) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method OneItem(): IEnumerator<int> "
        "    yield return 42; "
        "  end OneItem; "
        "  public static method Main(): int "
        "    using var e := OneItem(); "
        "    var moved := e.MoveNext(); "
        "    if moved then return 1; end; "
        "    return 0; "
        "  end Main; end C;"), 1);
}

// ---- B: Manual MoveNext/Current loop summing 3 values --------------------------
// per §29.2 / §29.3: explicitly call MoveNext() and Current to drive an iterator.
// Exercises both vtable slots 2 (MoveNext → i1) and 3 (Current → int).
BEZEL_TEST(itable_bool_return_494, manual_movenext_current_loop_sum) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Seq(): IEnumerator<int> "
        "    yield return 1; yield return 2; yield return 3; "
        "  end Seq; "
        "  public static method Main(): int "
        "    using var e := Seq(); "
        "    var s := 0; "
        "    while e.MoveNext() do "
        "      s := s + e.Current; "
        "    end; "
        "    return s; "
        "  end Main; end C;"), 6);
}

// ---- C: Annotated `var b: bool := e.MoveNext()` --------------------------------
// per §29.2: when the local IS annotated as bool the declared type gates the
// slot. After the fix the call result is i1, the slot is i1, no type mismatch.
BEZEL_TEST(itable_bool_return_494, annotated_bool_local_movenext) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method OneItem(): IEnumerator<int> "
        "    yield return 99; "
        "  end OneItem; "
        "  public static method Main(): int "
        "    using var e := OneItem(); "
        "    var b: bool := e.MoveNext(); "
        "    if b then return 1; end; "
        "    return 0; "
        "  end Main; end C;"), 1);
}

// ---- D: user-defined interface with a bool-returning method, via interface var -
// per §13 / CG35-itable: this is the complementary itable path — a user-defined
// interface (with a real TypeDecl) whose method returns bool, called through an
// interface-typed variable. The itable dispatch path in emitMethodCall (not the
// vtable-direct path fixed above) must return i1. Regression guard.
BEZEL_TEST(itable_bool_return_494, user_iface_bool_method_via_iface_var) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IBoolMethod "
        "  method IsReady(): bool; "
        "end IBoolMethod; "
        "class ReadyImpl : IBoolMethod "
        "  public method IsReady(): bool return true; end IsReady; "
        "  destructor end destructor; "
        "end ReadyImpl; "
        "class C "
        "  public static method Check(b: IBoolMethod): int "
        "    var ok := b.IsReady(); "
        "    if ok then return 1; end; "
        "    return 0; "
        "  end Check; "
        "  public static method Main(): int "
        "    using var r := new ReadyImpl(); "
        "    return Check(r); "
        "  end Main; end C;"), 1);
}

// ---- E (control): for var x in iterator still works ----------------------------
// per §29.2: `for var x in OneItem()` uses the vtable-slot-2 direct path
// (lowerIEnumerableBuiltinFor), which was already correct. Regression guard.
BEZEL_TEST(itable_bool_return_494, control_for_in_iterator_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method OneTwoThree(): IEnumerable<int> "
        "    yield return 1; yield return 2; yield return 3; "
        "  end OneTwoThree; "
        "  public static method Main(): int "
        "    var s := 0; for var x in OneTwoThree() do s:=s+x; end; return s; "
        "  end Main; end C;"), 6);
}

// ---- F (control): vtable bool method on a concrete class var -------------------
// per §12.1 / §12.5: a virtual bool method called on a CLASS-typed var goes
// through the vtable (not itable), so the result is i1. Regression guard.
BEZEL_TEST(itable_bool_return_494, control_vtable_bool_method_class_var) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public virtual method IsOk(): bool return false; end IsOk; "
        "  destructor end destructor; "
        "end Base; "
        "class Derived : Base "
        "  public override method IsOk(): bool return true; end IsOk; "
        "  destructor end destructor; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Derived(); "
        "    var b := d.IsOk(); "
        "    if b then return 1; end; "
        "    return 0; "
        "  end Main; end C;"), 1);
}
