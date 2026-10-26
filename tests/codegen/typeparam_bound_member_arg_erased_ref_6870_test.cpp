// Codegen — an INTERFACE-MEMBER CALL on a type parameter bounded to
// REFERENCE types, inside a generic CLASS or a `virtual` generic method,
// must compile (§14.2, §11.8; spec v0.343).
//
// THE DEFECT (#6870).  A generic declaration is emitted TWICE: once per
// instantiation (the monomorphized body) and once UNSPECIALIZED — the
// "template" — with its type parameters erased to the `int` (i32)
// placeholder.  §11.8 leaves the realization strategy open ("An
// implementation may realize this by per-instantiation specialization or by
// shared code carrying a per-instantiation dispatch table … so long as the
// observable binding is the conforming type's member"), so the erased
// template is an implementation choice — but it still has to lower, and the
// erasure decides whether the member call inside it can lower at all:
//
//   * a `T`-typed RECEIVER in the template was an i32, so the interface /
//     vtable dispatch had nothing to dispatch on — issue #484's
//     "receiver is not a pointer" guard refused it, `lowerExpr` returned
//     null, and the compile stopped on the internal diagnostic
//     `internal: lowerExpr returned null for call argument in
//     buildOrderedArgs — #1386`.  That diagnostic names an issue number and
//     is not a located, spec-cited diagnostic: a spec-CONFORMING program
//     was REFUSED TO COMPILE.
//
//   * the erasure was also wrong on its own terms.  §14.2's constraint table
//     confines `where T: class` to reference types and `where T: SomeClass`
//     to that class and its descendants; `int` is a legal substitution for
//     NEITHER.  The template's `T` is therefore a REFERENCE, and the i32
//     placeholder is not a conservative stand-in for it — it is a
//     representation the parameter can never hold.
//
// THE FIX.  A type parameter whose constraint set admits only reference
// types is erased to a `ptr` in the erased template, so the receiver is a
// pointer and the member call dispatches through the runtime itable
// (`bz_itable_lookup`) exactly as the per-instantiation specialization
// does.  The erased template's SIGNATURE, its BODY and the ERASED LAYOUT of
// its declaring type must all use the same representation, or the IR stops
// verifying — see the two-level case below, which is where the layout, the
// signature and the body have to agree on `T?` (for a reference-constrained
// `T`, §7.1's nullable form of a REFERENCE type) rather than on the
// `{i8, i32}` tagged placeholder of a nullable VALUE.
//
// ⚠ THE DISCRIMINATOR IS THE SCOPE, NOT THE NAME (the fix-forward finding on
// the first attempt at this fix).  A type parameter SHADOWS an
// always-available builtin name — §2.3: the standard exception types are
// always-available and "every other always-available name may be shadowed by
// local declarations under the normal name-resolution rules".  So in
// `class C<Exception> where Exception: INumber<Exception>`, the method's
// `where T: Exception` is a §14.2 TYPE-PARAMETER bound ("another type
// parameter — of the same declaration, or of the enclosing type"), which has
// subtype semantics and admits VALUE types ("at `U := long` it admits `long`
// alone") — NOT a class bound on the builtin `Exception`.
//
// ⇒⇒ A predicate that tested the bound's NAME against the builtin exception
// table erased that `T` to a `ptr`, while the specialization's `T` was an
// `i32` (the class is instantiated at `C<int>`), and the IR stopped
// verifying: a spec-conforming program that compiled and printed the right
// answer was REFUSED TO COMPILE.  MEASURED: the two bounds — the builtin
// class's and the user's type parameter's — resolve to TypeRefs IDENTICAL in
// every observable field (`declaration == nullptr`, the same syntactic head
// name, an AST node, no generic arguments), so no test on the resolved type
// can tell them apart.  Only the SCOPE can.  The three
// `typeparam_bound_shadowing_*` rows below pin that shape.
//
// WHY THE CALL REACHES THE TEMPLATE AT ALL (the mechanism the brief asked
// for).  Codegen infers a generic method's type arguments itself
// (`unifyMethodReturnTypeArg`, LlvmBackend_Calls2.cpp): it binds a method
// type parameter only when the declared parameter type is a BARE type
// parameter, or a builtin generic with the SAME name (`Array<T>` vs
// `Array<Widget>`).  A stdlib-interface parameter (`IEnumerable<T>`,
// `IList<T>`, …) has a real `TypeSymbol` while the argument (`Array<Widget>`)
// is a decl-less builtin, so the declaration-identity test fails, `T` stays
// unbound, the "force specialization" test never fires, and the call is
// emitted against the UNSPECIALIZED template.  Sema's `unifyArgSubst`
// implements the correct §14.5 rule ("the unique instantiation of G that the
// argument's type implements or extends"); codegen has a weaker duplicate of
// it.  That gap is a separate defect (#6931 tracks the vtable-slot half) and
// is NOT closed here — what is closed here is that the template, once
// reached, LOWERS.
//
// ⚠ THE REMAINING GAP (why this is a NARROW fix).  An INTERFACE-ONLY bound
// (`where T: IFoo`) is deliberately NOT given the reference erasure: §14.2
// admits "classes implementing it, records and enums implementing it,
// interfaces extending it, and primitives conforming through their
// standard-library declarations or the compiler-provided conformances", so
// such a parameter may hold a VALUE type — a class substitution arrives as
// a pointer, a record substitution BY VALUE (MEASURED on `B$V$spec$Dog` vs
// `B$V$spec$Pt`) — and there is no single uniform erased representation.
// Erasing it as a pointer would be UNSOUND.  The interface-only-bound shape
// therefore still stops on the #1386 internal diagnostic; that is the rest
// of #6870 and it is not closed by this change.
//
// Every test here produces the spec-stated value after the fix.  The rows
// that pin the #1386 shape FAIL TO COMPILE before it (the internal
// diagnostic, or an IR-verification failure); the CONTROL rows compile and
// run before it as well, and pin that the fix does not change them.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bezel::test::JitResult jit(const std::string& prog) { return runAot(prog); }

}  // namespace

// ── the generic-CLASS shape: `class Holder<T> where T: Exception, IMark`
//    calling `IMark.Mark` on a `T`-typed parameter. §14.2 (the constraint
//    table admits only reference types), §14.5 (the instantiation), §3.6
//    rule 2 (per-instantiation evaluation). The `Mark` implementation
//    returns 7, so the program prints 7. ─────────────────────────────────────
BEZEL_TEST(typeparam_bound_member_arg_erased_ref_6870,
           generic_class_member_call_on_bounded_t_spec_14_2) {
    auto r = jit(
        "module M; "
        "public interface IMark "
        "  method Mark(): int; "
        "end IMark; "
        "public class MarkedError: Exception, IMark "
        "  public constructor() base(\"marked\"); end constructor; "
        "  public method Mark(): int return 7; end Mark; "
        "end MarkedError; "
        "public class Holder<T> where T: Exception, IMark "
        "  public method Show(item: T) "
        "    Record(item.Mark()); "
        "  end Show; "
        "  public static method Record(n: int) "
        "    Console.WriteLine($\"{n}\"); "
        "  end Record; "
        "end Holder; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder<MarkedError>(); "
        "    h.Show(new MarkedError()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// ── the `virtual` generic-METHOD shape: the same bound on a method's own
//    type parameter, called through a base-class reference. §12 (vtable
//    dispatch), §14.2 (the constraint table). ────────────────────────────────
BEZEL_TEST(typeparam_bound_member_arg_erased_ref_6870,
           virtual_generic_method_member_call_on_bounded_t_spec_14_2) {
    auto r = jit(
        "module M; "
        "public interface IMark "
        "  method Mark(): int; "
        "end IMark; "
        "public class MarkedError: Exception, IMark "
        "  public constructor() base(\"marked\"); end constructor; "
        "  public method Mark(): int return 7; end Mark; "
        "end MarkedError; "
        "public class B "
        "  public constructor() end constructor; "
        "  public virtual method V<T>(x: T): int where T: Exception, IMark "
        "    Record(x.Mark()); "
        "    return 1; "
        "  end V; "
        "  public static method Record(n: int) "
        "    Console.WriteLine($\"{n}\"); "
        "  end Record; "
        "end B; "
        "public class Program "
        "  public static method Main(): int "
        "    var d := new MarkedError(); "
        "    var b := new B(); "
        "    _ := b.V(d); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// ── TWO LEVELS, with a NULLABLE type parameter (§7.1). This is the case
//    that forces the erased template's SIGNATURE, the ERASED LAYOUT of the
//    declaring type and the BODY to agree on one representation of `T?`:
//    `Inner<T>`'s field `T?` is lowered in `Inner`'s erased LAYOUT, while
//    `Inner$Head`'s return type is lowered in its erased SIGNATURE, and the
//    pseudo-specialization built from inside `Outer<U>`'s template carries
//    the substitution `T → U`.  A disagreement is an IR-verification failure
//    ("Function return type does not match operand type of return inst!"),
//    not a wrong answer.  The program returns 3 — the value is null. ───────
BEZEL_TEST(typeparam_bound_member_arg_erased_ref_6870,
           two_level_nullable_field_value_spec_7_10) {
    auto r = jit(
        "module M; import System.Diagnostics; "
        "class Res public n: int := 0; end Res; "
        "class Inner<T> where T: class "
        "  private _v: T? := null; "
        "  public method Head(): T? return _v; end Head; "
        "end Inner; "
        "class OuterU<U> where U: class "
        "  private owned g: Inner<U> := new Inner<U>(); "
        "  public method Head(): U? return g.Head(); end Head; "
        "end OuterU; "
        "class C public static method Main(): int "
        "  using var o := new OuterU<Res>(); "
        "  var s := o.Head(); "
        "  if s = null then return 3; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ── The same generic-CLASS shape under the CHECKED oracle: generated-code
//    ASan + LeakSanitizer. The change moves the erased LAYOUT of the generic
//    class, so the layout has to survive a sanitizer as well as the value
//    check above. `runAotChecked` fails the row on a leak, a double free or
//    a use-after-free before the printed value is compared. Prints 7 and
//    exits 0. ───────────────────────────────────────────────────────────────
BEZEL_TEST(typeparam_bound_member_arg_erased_ref_6870,
           generic_class_bounded_t_checked_leak_free_spec_14_2) {
    auto r = runAotChecked(
        "module M; "
        "public interface IMark "
        "  method Mark(): int; "
        "end IMark; "
        "public class MarkedError: Exception, IMark "
        "  public constructor() base(\"marked\"); end constructor; "
        "  public method Mark(): int return 7; end Mark; "
        "end MarkedError; "
        "public class Holder<T> where T: Exception, IMark "
        "  public method Show(item: T) "
        "    Record(item.Mark()); "
        "  end Show; "
        "  public static method Record(n: int) "
        "    Console.WriteLine($\"{n}\"); "
        "  end Record; "
        "end Holder; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder<MarkedError>(); "
        "    h.Show(new MarkedError()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// ── CONTROL: the other stdlib-INTERFACE parameters of the same shape
//    (`IList<T>`, `IReadOnlyList<T>` alongside `IEnumerable<T>` above).
//    Each is a `where T: class` parameter whose DECLARED type is an
//    interface, so the §14.5 inference gap sends the call to the
//    unspecialized template, and the template has to keep working. Prints
//    "1 1 1" — one element each. ───────────────────────────────────────────
BEZEL_TEST(typeparam_bound_member_arg_erased_ref_6870,
           control_stdlib_interface_parameter_variants_spec_14_5) {
    auto r = jit(
        "module M; "
        "public class Widget "
        "  public constructor() end constructor; "
        "  public method Id(): int return 9; end Id; "
        "end Widget; "
        "public static class H "
        "  public static method F<T>(items: IEnumerable<T>): int where T: class "
        "    var n := 0; "
        "    for var x in items do n := n + 1; end; "
        "    return n; "
        "  end F; "
        "  public static method G<T>(items: IList<T>): int where T: class "
        "    var n := 0; "
        "    for var x in items do n := n + 1; end; "
        "    return n; "
        "  end G; "
        "  public static method K<T>(items: IReadOnlyList<T>): int where T: class "
        "    var n := 0; "
        "    for var x in items do n := n + 1; end; "
        "    return n; "
        "  end K; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var arr: Array<Widget> := [new Widget()]; "
        "    Console.WriteLine($\"{H.F(arr)} {H.G(arr)} {H.K(arr)}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1 1 1\n");
}

// ── CONTROL: a USER-DECLARED CLASS bound (`where T: Base`). §14.2 admits
//    `Base` and its descendants only — every legal substitution is a
//    reference, so the erased template's `T` is a pointer and the member
//    call on it dispatches through the vtable (§12). Prints 5. ─────────────
BEZEL_TEST(typeparam_bound_member_arg_erased_ref_6870,
           control_user_class_bound_spec_14_2) {
    auto r = jit(
        "module M; "
        "public class Base "
        "  public constructor() end constructor; "
        "  public method Id(): int return 5; end Id; "
        "end Base; "
        "public class Derived: Base "
        "  public constructor() end constructor; "
        "end Derived; "
        "public static class H "
        "  public static method F<T>(x: T): int where T: Base "
        "    return x.Id(); "
        "  end F; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var d := new Derived(); "
        "    Console.WriteLine($\"{H.F(d)}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// ── THE COLLISION SHAPE (#6870, fix-forward): a type parameter whose NAME
//    shadows an always-available builtin (`Exception`, `IOException`, …).
//
//    §2.3: the always-available names — the §31.4 standard exception types
//    among them — "may be shadowed by local declarations under the normal
//    name-resolution rules", and a type parameter is such a declaration.
//    §14.2 ("Type-parameter bounds"): a constraint whose bound is "another
//    type parameter — of the same declaration, or of the enclosing type" —
//    "has subtype semantics", and it admits VALUE types: "at `U := long` it
//    admits `long` alone".
//
//    ⇒⇒ So in `class C<Exception> where Exception: INumber<Exception>`, the
//    method's `where T: Exception` is a TYPE-PARAMETER bound, NOT a class
//    bound on the builtin `Exception` class. The parameter `T` admits `int`
//    (the class is instantiated at `C<int>`), so the erased template's `T`
//    must NOT be erased to a pointer — the specialization's `T` is an `i32`
//    and the two representations have to agree.
//
//    The erased template's `T` is what the interface member call dispatches
//    through, so getting this wrong does not merely pick a wrong
//    representation: the IR stops verifying ("Call parameter type does not
//    match function signature!") and a spec-conforming program is REFUSED TO
//    COMPILE.
//
//    The three rows below differ ONLY in the type parameter's NAME: the two
//    that shadow a builtin exception type and the control that shadows
//    nothing. All three must compile and print `sum=60`. ──────────────────
BEZEL_TEST(typeparam_bound_member_arg_erased_ref_6870,
           typeparam_bound_shadowing_builtin_exception_name_spec_14_2) {
    auto r = jit(
        "module M; "
        "public class C<Exception> where Exception: INumber<Exception> "
        "  public static method F<T>(items: IEnumerable<T>): int where T: Exception "
        "    var n := 0; "
        "    for var x in items do "
        "      n := n + x; "
        "    end; "
        "    return n; "
        "  end F; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    var arr: Array<int> := [10, 20, 30]; "
        "    var n := C<int>.F(arr); "
        "    Console.WriteLine($\"sum={n}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "sum=60\n");
}

// ── The same shape with the SECOND builtin exception name — the erasure
//    predicate must not be a patch that names `Exception` alone. ────────────
BEZEL_TEST(typeparam_bound_member_arg_erased_ref_6870,
           typeparam_bound_shadowing_builtin_exception_name_ioexception_spec_14_2) {
    auto r = jit(
        "module M; "
        "public class C<IOException> where IOException: INumber<IOException> "
        "  public static method F<T>(items: IEnumerable<T>): int where T: IOException "
        "    var n := 0; "
        "    for var x in items do "
        "      n := n + x; "
        "    end; "
        "    return n; "
        "  end F; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    var arr: Array<int> := [10, 20, 30]; "
        "    var n := C<int>.F(arr); "
        "    Console.WriteLine($\"sum={n}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "sum=60\n");
}

// ── The CONTROL: the same program with a type parameter named after a
//    NON-builtin. It must keep compiling and printing the same value — the
//    shape that worked before the collision and must keep working. ─────────
BEZEL_TEST(typeparam_bound_member_arg_erased_ref_6870,
           typeparam_bound_shadowing_non_builtin_name_control_spec_14_2) {
    auto r = jit(
        "module M; "
        "public class C<Widget> where Widget: INumber<Widget> "
        "  public static method F<T>(items: IEnumerable<T>): int where T: Widget "
        "    var n := 0; "
        "    for var x in items do "
        "      n := n + x; "
        "    end; "
        "    return n; "
        "  end F; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    var arr: Array<int> := [10, 20, 30]; "
        "    var n := C<int>.F(arr); "
        "    Console.WriteLine($\"sum={n}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "sum=60\n");
}

// ── CONTROL: the shape the rejected attempt broke. A `where T: class`
//    parameter whose DECLARED type is a stdlib INTERFACE (`IEnumerable<T>`)
//    — the §14.5 inference gap sends the call to the unspecialized
//    template, and the template has to keep working. It prints 1 (one
//    element). ───────────────────────────────────────────────────────────────
BEZEL_TEST(typeparam_bound_member_arg_erased_ref_6870,
           control_interface_parameter_shape_spec_14_5) {
    auto r = jit(
        "module M; "
        "public class Widget "
        "  public constructor() end constructor; "
        "  public method Id(): int return 9; end Id; "
        "end Widget; "
        "public static class H "
        "  public static method F<T>(items: IEnumerable<T>): int where T: class "
        "    var n := 0; "
        "    for var x in items do n := n + 1; end; "
        "    return n; "
        "  end F; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var arr: Array<Widget> := [new Widget()]; "
        "    Console.WriteLine($\"{H.F(arr)}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

// ── CONTROL: the same call reached through a METHOD REFERENCE (§12.1) —
//    the erased template's body runs the interface call through a captured
//    cell. It prints "go", "after", "n=5". ─────────────────────────────────
BEZEL_TEST(typeparam_bound_member_arg_erased_ref_6870,
           control_method_reference_form_spec_12_1) {
    auto r = jit(
        "module M; "
        "public interface IMark "
        "  method Mark(): int; "
        "end IMark; "
        "public class Dog: IMark "
        "  public constructor() end constructor; "
        "  public method Mark(): int return 3; end Mark; "
        "end Dog; "
        "public static class U "
        "  public static method G<T>(x: T): int where T: IMark "
        "    return x.Mark() + 1; "
        "  end G; "
        "  public static method Log(s: string) Console.WriteLine(s); end Log; "
        "end U; "
        "public class Box<K> where K: IMark "
        "  private k: K; "
        "  public constructor(k: K) this.k := k; end constructor; "
        "  public method Go(): int "
        "    U.Log(\"go\"); "
        "    var r := U.G(k); "
        "    U.Log(\"after\"); "
        "    return 5; "
        "  end Go; "
        "end Box; "
        "public class Prog "
        "  public static method Main(): int "
        "    var b := new Box<Dog>(new Dog()); "
        "    var f := b.Go; "
        "    var n := f(); "
        "    Console.WriteLine($\"n={n}\"); "
        "    return 0; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "go\nafter\nn=5\n");
}

// ── CONTROL: a type parameter instantiated at a RECORD (a VALUE type).
//    §14.2 admits a record for an interface-bounded parameter, and a record
//    is passed BY VALUE — the reference erasure must not turn it into a
//    pointer. It prints 42. ────────────────────────────────────────────────
BEZEL_TEST(typeparam_bound_member_arg_erased_ref_6870,
           control_record_type_param_keeps_its_layout_spec_14_2) {
    auto r = jit(
        "module M; "
        "public interface IMark "
        "  method Mark(): int; "
        "end IMark; "
        "public record Pt(X: int): IMark "
        "  public method Mark(): int return 42; end Mark; "
        "end Pt; "
        "public class B "
        "  public constructor() end constructor; "
        "  public virtual method V<T>(x: T): int where T: IMark "
        "    return x.Mark(); "
        "  end V; "
        "end B; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new Pt(5); "
        "    var b := new B(); "
        "    Console.WriteLine($\"{b.V(p)}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}
