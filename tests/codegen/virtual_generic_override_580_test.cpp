// Codegen — virtual GENERIC method override passes a corrupt argument for
// non-int type arguments (issue #580).
//
// Root cause: a virtual generic method is compiled ONCE as a template with each
// type parameter erased to the i32 placeholder, so the vtable slot's function
// signature is fixed to `(this, i32) -> ...`. A call `d.Show<string>("hello")`
// builds an indirect call whose signature is the CONCRETE `(this, ptr) -> ptr`
// (because methodFnType lowers `T` through the active substitution), but it loads
// the i32-erased template from the vtable slot. The string pointer (64-bit) is
// passed where the callee reads an i32 (32-bit) — a corrupt argument
// (`Derived(4345072)`). And the template body formats `x:T` as an int (T→i32),
// so even a width-correct call would be wrong. `Show<int>` worked only by luck:
// T→i32 matched the placeholder.
//
// Fix: a virtual generic class method dispatches by the receiver's dynamic
// descriptor (vtable[0] = td$C) to a per-(class, type-arg) MONOMORPHIZED body —
// one specialization per overriding class in the hierarchy, with the static
// class's own specialization as the default branch (always sound: the dynamic
// type is a subtype of the static type).
//
// Spec references:
//   §14.1 / §14.2 — generic methods are monomorphized at each call site; all
//     type-parameter references resolve to concrete types.
//   §12 — inheritance and virtual dispatch: an override is selected by the
//     receiver's runtime type.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Two-class hierarchy: Base virtual Show<T>, Derived overrides it. Each test
// appends a Main that compares the produced string and returns an int code.
const char* kHierarchy =
    "module M; "
    "class Base "
    "  public virtual method Show<T>(x: T): string => $\"Base({x})\"; "
    "end Base; "
    "class Derived: Base "
    "  public override method Show<T>(x: T): string => $\"Derived({x})\"; "
    "end Derived; ";
}  // namespace

// ── §14.1 / §12 / #580: Derived-typed receiver (exact static type) ────────────

// Show<int> on a Derived-typed receiver — the control case; worked before too.
BEZEL_TEST(codegen_virtual_generic_580, derived_recv_int_14_1_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kHierarchy) +
        "class App "
        "  public static method Main(): int "
        "    using var d: Derived := new Derived(); "
        "    if d.Show<int>(99) = \"Derived(99)\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 1);
}

// Show<string> on a Derived-typed receiver — garbage before the fix.
BEZEL_TEST(codegen_virtual_generic_580, derived_recv_string_14_1_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kHierarchy) +
        "class App "
        "  public static method Main(): int "
        "    using var d: Derived := new Derived(); "
        "    if d.Show<string>(\"hi\") = \"Derived(hi)\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 1);
}

// Show<double> on a Derived-typed receiver — garbage before the fix.
BEZEL_TEST(codegen_virtual_generic_580, derived_recv_double_14_1_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kHierarchy) +
        "class App "
        "  public static method Main(): int "
        "    using var d: Derived := new Derived(); "
        "    if d.Show<double>(2.5) = \"Derived(2.5)\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 1);
}

// ── §12 / #580: Base-typed receiver holding a Derived (dynamic dispatch) ───────

// The override must be selected by the RUNTIME type AND monomorphized for T.
BEZEL_TEST(codegen_virtual_generic_580, base_recv_derived_string_dispatch_12_14_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kHierarchy) +
        "class App "
        "  public static method Main(): int "
        "    using var d: Derived := new Derived(); "
        "    var b: Base := d; "
        "    if b.Show<string>(\"hi\") = \"Derived(hi)\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 1);
}

BEZEL_TEST(codegen_virtual_generic_580, base_recv_derived_double_dispatch_12_14_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kHierarchy) +
        "class App "
        "  public static method Main(): int "
        "    using var d: Derived := new Derived(); "
        "    var b: Base := d; "
        "    if b.Show<double>(2.5) = \"Derived(2.5)\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 1);
}

BEZEL_TEST(codegen_virtual_generic_580, base_recv_derived_int_dispatch_12_14_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kHierarchy) +
        "class App "
        "  public static method Main(): int "
        "    using var d: Derived := new Derived(); "
        "    var b: Base := d; "
        "    if b.Show<int>(99) = \"Derived(99)\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 1);
}

// ── §12 / #580: Base-typed receiver holding a Base (the default branch) ────────

// A Base instance through a Base reference must run Base's own override.
BEZEL_TEST(codegen_virtual_generic_580, base_recv_base_string_default_12_14_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kHierarchy) +
        "class App "
        "  public static method Main(): int "
        "    using var o: Base := new Base(); "
        "    var b: Base := o; "
        "    if b.Show<string>(\"hi\") = \"Base(hi)\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 1);
}

// ── §12 / #580: 3-level hierarchy; the grandchild inherits Derived's override ──

BEZEL_TEST(codegen_virtual_generic_580, grandchild_inherits_derived_override_12_14_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kHierarchy) +
        "class GrandChild: Derived "
        "end GrandChild; "
        "class App "
        "  public static method Main(): int "
        "    using var g: GrandChild := new GrandChild(); "
        "    var b: Base := g; "
        "    if b.Show<string>(\"hi\") = \"Derived(hi)\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 1);
}

// ── Regression guards (the fix must not perturb adjacent paths) ───────────────

// §12: a NON-generic virtual override still dispatches correctly via the vtable.
BEZEL_TEST(codegen_virtual_generic_580, nongeneric_virtual_override_regression_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public virtual method Name(): string => \"Base\"; "
        "end Base; "
        "class Derived: Base "
        "  public override method Name(): string => \"Derived\"; "
        "end Derived; "
        "class App "
        "  public static method Main(): int "
        "    using var d: Derived := new Derived(); "
        "    var b: Base := d; "
        "    if b.Name() = \"Derived\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 1);
}

// §14.1: a NON-virtual generic method (no override surface) still monomorphizes
// correctly for string / int via the existing inline-specialization path.
BEZEL_TEST(codegen_virtual_generic_580, nonvirtual_generic_method_regression_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Plain "
        "  public method Show<T>(x: T): string => $\"Plain({x})\"; "
        "end Plain; "
        "class App "
        "  public static method Main(): int "
        "    using var p: Plain := new Plain(); "
        "    if p.Show<string>(\"hi\") = \"Plain(hi)\" "
        "       and p.Show<int>(7) = \"Plain(7)\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 1);
}

// §14.1 / §12 / #580: distinct type arguments on the SAME receiver in one body
// must each get their own monomorphization (no specialization-cache collision).
BEZEL_TEST(codegen_virtual_generic_580, multiple_typeargs_same_receiver_14_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kHierarchy) +
        "class App "
        "  public static method Main(): int "
        "    using var d: Derived := new Derived(); "
        "    var b: Base := d; "
        "    var s := b.Show<string>(\"x\"); "
        "    var i := b.Show<int>(1); "
        "    var f := b.Show<double>(0.5); "
        "    if s = \"Derived(x)\" and i = \"Derived(1)\" "
        "       and f = \"Derived(0.5)\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 1);
}

// ── §13 / §14.1 / #580: the SAME defect via an interface receiver ─────────────
// A generic INTERFACE method dispatches through the itable slot (also fixed to
// the i32 template). The fix routes it through the same descriptor dispatch.

namespace {
// Three classes implement IShow<T>; an interface-typed receiver must reach each
// implementer's own monomorphized body for any type argument.
const char* kIface =
    "module M; "
    "interface IShow "
    "  method Show<T>(x: T): string; "
    "end IShow; "
    "class A: IShow "
    "  public method Show<T>(x: T): string => $\"A({x})\"; "
    "end A; "
    "class B: IShow "
    "  public method Show<T>(x: T): string => $\"B({x})\"; "
    "end B; ";
}  // namespace

// Interface-typed receiver, string type arg — garbage before the fix.
BEZEL_TEST(codegen_virtual_generic_580, iface_recv_string_13_14_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kIface) +
        "class App "
        "  public static method Main(): int "
        "    using var a: A := new A(); "
        "    var i: IShow := a; "
        "    if i.Show<string>(\"hi\") = \"A(hi)\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 1);
}

// Interface-typed receiver, distinct implementers + distinct type args.
BEZEL_TEST(codegen_virtual_generic_580, iface_recv_multi_impl_dispatch_13_14_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kIface) +
        "class App "
        "  public static method Main(): int "
        "    using var a: A := new A(); "
        "    using var b: B := new B(); "
        "    var ia: IShow := a; "
        "    var ib: IShow := b; "
        "    if ia.Show<double>(1.5) = \"A(1.5)\" "
        "       and ib.Show<string>(\"q\") = \"B(q)\" "
        "       and ib.Show<int>(8) = \"B(8)\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 1);
}

// ── §12 / §14.1 / #580: a VOID virtual generic method must dispatch + run once ─
// (regression guard for the void-sentinel: returning nullptr would make the
// caller fall through to the vtable path and emit a second, broken call). The
// override mutates a counter so we observe exactly one dispatched execution.
BEZEL_TEST(codegen_virtual_generic_580, void_virtual_generic_dispatch_once_12_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public n: int; "
        "  public virtual method Bump<T>(x: T) "
        "    n := n + 1; "
        "  end Bump; "
        "end Base; "
        "class Derived: Base "
        "  public override method Bump<T>(x: T) "
        "    n := n + 10; "
        "  end Bump; "
        "end Derived; "
        "class App "
        "  public static method Main(): int "
        "    using var d: Derived := new Derived(); "
        "    var b: Base := d; "
        "    b.Bump<string>(\"a\"); "   // +10 (Derived), exactly once
        "    b.Bump<double>(1.5); "     // +10 (Derived)
        "    return d.n; "              // 20 if each ran once via Derived
        "  end Main; "
        "end App;"), 20);
}

// ── §12 / §14.1 / #580: abstract generic virtual method, two concrete subtypes ─
BEZEL_TEST(codegen_virtual_generic_580, abstract_generic_virtual_subclasses_12_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "abstract class Shape "
        "  public abstract method Describe<T>(x: T): string; "
        "end Shape; "
        "class Circle: Shape "
        "  public override method Describe<T>(x: T): string => $\"Circle({x})\"; "
        "end Circle; "
        "class Square: Shape "
        "  public override method Describe<T>(x: T): string => $\"Square({x})\"; "
        "end Square; "
        "class App "
        "  public static method Main(): int "
        "    using var c: Circle := new Circle(); "
        "    using var s: Square := new Square(); "
        "    var sh1: Shape := c; "
        "    var sh2: Shape := s; "
        "    if sh1.Describe<string>(\"a\") = \"Circle(a)\" "
        "       and sh2.Describe<double>(1.5) = \"Square(1.5)\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 1);
}
