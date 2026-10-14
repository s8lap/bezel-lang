// Codegen -- §11.2 / §12.1 (v0.338): a call through an interface runs the receiver's
// implementation of the member AT the interface instantiation the receiver is typed at.
// §11.2: "the implementing member must match the interface member's signature exactly --
// same name, same parameter types (after substituting the interface's type arguments)".
// §12.1: an `override` replaces the base's slot, so the most-derived override runs.
// Issue #6307: interface dispatch of a generic method (Emitter::emitVirtualGenericDispatch,
// interface-parent branch) compared only shared descriptors, which a generic receiver
// (per-instantiation descriptor, #1843) never carries, and picked each class's witness
// without the interface's type arguments, so every unmatched receiver ran the LAST
// implementer's body (right or wrong by declaration order), and a class implementing
// `I<int>` and `I<string>` ran one overload for both.
// Each row's observable is captured stdout; rows that could depend on declaration order
// come in both orders.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// The issue's repro 1: generic implementers `B<T>` and `C<X>: B<X>`. Pre-fix "1\n1\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, issue_generic_implementers_11_2_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class B<T>: I<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class C<X>: B<X> public constructor() end constructor; public override method Q<V>(e: V, t: X): int => 2; end C; "
        "class P public static method Main(): int var i: I<string> := new C<string>(); var j: I<string> := new B<string>(); "
        " Console.WriteLine(i.Q<int>(1,\"s\")); Console.WriteLine(j.Q<int>(1,\"s\")); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n1\n");
}

// Repro 1 with the override class declared first. Pre-fix "1\n1\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, issue_generic_implementers_override_first_11_2_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class C<X>: B<X> public constructor() end constructor; public override method Q<V>(e: V, t: X): int => 2; end C; "
        "class B<T>: I<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class P public static method Main(): int var i: I<string> := new C<string>(); var j: I<string> := new B<string>(); "
        " Console.WriteLine(i.Q<int>(1,\"s\")); Console.WriteLine(j.Q<int>(1,\"s\")); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n1\n");
}

// The issue's repro 2: `C: I<int>, I<string>` with two `Q` overloads. Pre-fix "7\n7\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, issue_two_instantiations_11_2_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class C: I<int>, I<string> public constructor() end constructor; public method Q<V>(e: V, t: int): int => 7; public method Q<V>(e: V, t: string): int => 8; end C; "
        "class P public static method Main(): int var a: I<int> := new C(); var b: I<string> := new C(); "
        " Console.WriteLine(a.Q<int>(1,1)); Console.WriteLine(b.Q<int>(1,\"s\")); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "7\n8\n");
}

// Repro 2 with the clauses and overloads listed in the other order. Pre-fix "8\n8\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, issue_two_instantiations_reversed_11_2_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class C: I<string>, I<int> public constructor() end constructor; public method Q<V>(e: V, t: string): int => 8; public method Q<V>(e: V, t: int): int => 7; end C; "
        "class P public static method Main(): int var b: I<string> := new C(); var a: I<int> := new C(); "
        " Console.WriteLine(b.Q<int>(1,\"s\")); Console.WriteLine(a.Q<int>(1,1)); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "8\n7\n");
}

// The #6307 comment's m2 shape, `C: B<int>` declared after `B<T>`. Pre-fix "1\n1\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, m2_base_then_override_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class B<T>: I<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class C: B<int> public constructor() end constructor; public override method Q<V>(e: V, t: int): int => 2; end C; "
        "class P public static method Main(): int "
        " using var r2: I<int> := new B<int>(); using var r1: I<int> := new C(); "
        " Console.WriteLine(r2.Q<string>(\"x\",1)); Console.WriteLine(r1.Q<string>(\"x\",1)); "
        " return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "1\n2\n");
}

// m2 with `C: B<int>` declared before `B<T>`. Pre-fix "1\n1\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, m2_override_then_base_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class C: B<int> public constructor() end constructor; public override method Q<V>(e: V, t: int): int => 2; end C; "
        "class B<T>: I<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class P public static method Main(): int "
        " using var r2: I<int> := new B<int>(); using var r1: I<int> := new C(); "
        " Console.WriteLine(r2.Q<string>(\"x\",1)); Console.WriteLine(r1.Q<string>(\"x\",1)); "
        " return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "1\n2\n");
}

// m2 plus `D: B<string>` inheriting the base body. Pre-fix "1\n1\n1\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, m3_override_base_and_inheriting_sibling_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class B<T>: I<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class C: B<int> public constructor() end constructor; public override method Q<V>(e: V, t: int): int => 2; end C; "
        "class D: B<string> public constructor() end constructor; end D; "
        "class P public static method Main(): int "
        " using var r1: I<int> := new C(); using var r2: I<int> := new B<int>(); using var r3: I<string> := new D(); "
        " Console.WriteLine(r1.Q<string>(\"x\",1)); Console.WriteLine(r2.Q<string>(\"x\",1)); Console.WriteLine(r3.Q<int>(1,\"y\")); "
        " return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n1\n1\n");
}

// `B<S, T>: I<T>` (S unpinned by the interface) with `C: B<string, int>`. Pre-fix "1\n1\n1\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, two_param_base_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class B<S, T>: I<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class C: B<string, int> public constructor() end constructor; public override method Q<V>(e: V, t: int): int => 2; end C; "
        "class P public static method Main(): int "
        " var a: I<int> := new B<bool, int>(); var b: I<int> := new C(); var c: I<int> := new B<string, int>(); "
        " Console.WriteLine(a.Q<string>(\"x\",1)); Console.WriteLine(b.Q<string>(\"x\",1)); Console.WriteLine(c.Q<string>(\"x\",1)); "
        " return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "1\n2\n1\n");
}

// Same, override class declared first. Pre-fix "1\n1\n1\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, two_param_base_override_first_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class C: B<string, int> public constructor() end constructor; public override method Q<V>(e: V, t: int): int => 2; end C; "
        "class B<S, T>: I<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class P public static method Main(): int "
        " var a: I<int> := new B<bool, int>(); var b: I<int> := new C(); var c: I<int> := new B<string, int>(); "
        " Console.WriteLine(a.Q<string>(\"x\",1)); Console.WriteLine(b.Q<string>(\"x\",1)); Console.WriteLine(c.Q<string>(\"x\",1)); "
        " return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "1\n2\n1\n");
}

// `C: B<Array<int>>` through `I<Array<int>>`. Pre-fix "1\n1\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, array_argument_base_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class B<T>: I<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class C: B<Array<int>> public constructor() end constructor; public override method Q<V>(e: V, t: Array<int>): int => 2; end C; "
        "class P public static method Main(): int "
        " var xs: Array<int> := [1, 2]; "
        " var a: I<Array<int>> := new B<Array<int>>(); var b: I<Array<int>> := new C(); "
        " Console.WriteLine(a.Q<string>(\"x\", xs)); Console.WriteLine(b.Q<string>(\"x\", xs)); "
        " return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "1\n2\n");
}

// Same, override class declared first. Pre-fix "1\n1\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, array_argument_base_override_first_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class C: B<Array<int>> public constructor() end constructor; public override method Q<V>(e: V, t: Array<int>): int => 2; end C; "
        "class B<T>: I<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class P public static method Main(): int "
        " var xs: Array<int> := [1, 2]; "
        " var a: I<Array<int>> := new B<Array<int>>(); var b: I<Array<int>> := new C(); "
        " Console.WriteLine(a.Q<string>(\"x\", xs)); Console.WriteLine(b.Q<string>(\"x\", xs)); "
        " return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "1\n2\n");
}

// The call is in a generic class: `G<Z>.Run(i: I<Z>)`. Pre-fix "1\n1\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, generic_call_site_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class B<T>: I<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class C: B<int> public constructor() end constructor; public override method Q<V>(e: V, t: int): int => 2; end C; "
        "class G<Z> public constructor() end constructor; public method Run(i: I<Z>, z: Z): int => i.Q<string>(\"x\", z); end G; "
        "class P public static method Main(): int "
        " var g := new G<int>(); var a: I<int> := new B<int>(); var b: I<int> := new C(); "
        " Console.WriteLine(g.Run(a, 1)); Console.WriteLine(g.Run(b, 1)); "
        " return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "1\n2\n");
}

// Same, `G<Z>` and the override class declared before the base. Pre-fix "1\n1\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, generic_call_site_declared_first_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "class G<Z> public constructor() end constructor; public method Run(i: I<Z>, z: Z): int => i.Q<string>(\"x\", z); end G; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class C: B<int> public constructor() end constructor; public override method Q<V>(e: V, t: int): int => 2; end C; "
        "class B<T>: I<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class P public static method Main(): int "
        " var g := new G<int>(); var a: I<int> := new B<int>(); var b: I<int> := new C(); "
        " Console.WriteLine(g.Run(a, 1)); Console.WriteLine(g.Run(b, 1)); "
        " return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "1\n2\n");
}

// A second interface `J<T>` on the same base and override. Pre-fix "1\n1\n10\n10\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, second_interface_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "interface J<T> method R<U>(e: U, t: T): int; end J; "
        "class B<T>: I<T>, J<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; public virtual method R<U>(e: U, t: T): int => 10; end B; "
        "class C: B<int> public constructor() end constructor; public override method Q<V>(e: V, t: int): int => 2; public override method R<V>(e: V, t: int): int => 20; end C; "
        "class P public static method Main(): int "
        " var a: I<int> := new B<int>(); var b: I<int> := new C(); var c: J<int> := new B<int>(); var d: J<int> := new C(); "
        " Console.WriteLine(a.Q<string>(\"x\",1)); Console.WriteLine(b.Q<string>(\"x\",1)); Console.WriteLine(c.R<string>(\"x\",1)); Console.WriteLine(d.R<string>(\"x\",1)); "
        " return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "1\n2\n10\n20\n");
}

// `D: C<int>` overrides again, `E: C<int>` inherits `C`'s override. Pre-fix "1\n1\n1\n1\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, three_level_chain_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class B<T>: I<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class C<X>: B<X> public constructor() end constructor; public override method Q<V>(e: V, t: X): int => 2; end C; "
        "class D: C<int> public constructor() end constructor; public override method Q<W>(e: W, t: int): int => 3; end D; "
        "class E: C<int> public constructor() end constructor; end E; "
        "class P public static method Main(): int var a: I<int> := new B<int>(); var b: I<int> := new C<int>(); var c: I<int> := new D(); var d: I<int> := new E(); "
        " Console.WriteLine(a.Q<string>(\"x\",1)); Console.WriteLine(b.Q<string>(\"x\",1)); Console.WriteLine(c.Q<string>(\"x\",1)); Console.WriteLine(d.Q<string>(\"x\",1)); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "1\n2\n3\n2\n");
}

// A void generic method. Pre-fix "B 1 x\nB 1 x\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, void_method_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T); end I; "
        "class B<T>: I<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T) Console.WriteLine($\"B {t} {e}\"); end Q; end B; "
        "class C: B<int> public constructor() end constructor; public override method Q<V>(e: V, t: int) Console.WriteLine($\"C {t} {e}\"); end Q; end C; "
        "class P public static method Main(): int var a: I<int> := new B<int>(); var b: I<int> := new C(); "
        " a.Q<string>(\"x\", 1); b.Q<string>(\"x\", 1); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "B 1 x\nC 1 x\n");
}

// A GENERIC class listing `I<int>` and `I<string>`. Pre-fix "7\n7\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, generic_class_two_instantiations_11_2_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class C<X>: I<int>, I<string> public constructor() end constructor; public method Q<V>(e: V, t: int): int => 7; public method Q<V>(e: V, t: string): int => 8; end C; "
        "class P public static method Main(): int var a: I<int> := new C<bool>(); var b: I<string> := new C<bool>(); "
        " Console.WriteLine(a.Q<int>(1,1)); Console.WriteLine(b.Q<int>(1,\"s\")); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "7\n8\n");
}

// Both witnesses inherited from a non-generic base. Pre-fix "7\n7\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, inherited_overloads_two_instantiations_11_2_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class Base public constructor() end constructor; public method Q<V>(e: V, t: int): int => 7; public method Q<V>(e: V, t: string): int => 8; end Base; "
        "class C: Base, I<int>, I<string> public constructor() end constructor; end C; "
        "class P public static method Main(): int var a: I<int> := new C(); var b: I<string> := new C(); "
        " Console.WriteLine(a.Q<int>(1,1)); Console.WriteLine(b.Q<int>(1,\"s\")); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "7\n8\n");
}

// The override reads a base field, so its body must be specialized at the receiver's instantiation. Pre-fix "B:v:1\nB:w:2\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, generic_override_reads_field_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): string; end I; "
        "class B<T>: I<T> protected v: string; public constructor(v: string) this.v := v; end constructor; public virtual method Q<U>(e: U, t: T): string => $\"B:{v}:{t}\"; end B; "
        "class C<X>: B<X> public constructor(v: string) base(v); end constructor; public override method Q<V>(e: V, t: X): string => $\"C:{v}:{t}\"; end C; "
        "class P public static method Main(): int var a: I<int> := new B<int>(\"v\"); var b: I<int> := new C<int>(\"w\"); "
        " Console.WriteLine(a.Q<string>(\"x\", 1)); Console.WriteLine(b.Q<string>(\"x\", 2)); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "B:v:1\nC:w:2\n");
}

// A non-generic interface implemented by `G<X>` and `H`. Pre-fix "6\n6\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, non_generic_interface_generic_implementer_11_2_v0338) {
    auto r = runAot(
        "module M; "
        "interface J method Q<U>(e: U): int; end J; "
        "class G<X>: J public constructor() end constructor; public method Q<U>(e: U): int => 5; end G; "
        "class H: J public constructor() end constructor; public method Q<U>(e: U): int => 6; end H; "
        "class P public static method Main(): int var a: J := new G<string>(); var b: J := new H(); "
        " Console.WriteLine(a.Q<int>(1)); Console.WriteLine(b.Q<int>(1)); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "5\n6\n");
}

// 100 iterations over owned receivers. Pre-fix "200\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, loop_owned_receivers_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class B<T>: I<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class C: B<string> public constructor() end constructor; public override method Q<V>(e: V, t: string): int => 2; end C; "
        "class P public static method Main(): int var s := 0; "
        " for var k in 0..<100 do using var a: I<string> := new B<string>(); using var b: I<string> := new C(); s := s + a.Q<int>(k, \"a\") + b.Q<string>(\"q\", \"b\"); end for; "
        " Console.WriteLine(s); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "300\n");
}

// The #6307 comment's m2 repro verbatim (/var/tmp/bezel-scratch/r30-p12-rv-6301/q/m2.bzl):
// only a plain `B<int>` receiver, `C: B<int>` declared after `B<T>`. Right before the
// fix ("1\n", by declaration order); the rejected #6301 guard 78649198 printed "2\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, m2_plain_base_receiver_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class B<T>: I<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class C: B<int> public constructor() end constructor; public override method Q<V>(e: V, t: int): int => 2; end C; "
        "class P public static method Main(): int "
        " using var r2: I<int> := new B<int>(); "
        " Console.WriteLine(r2.Q<string>(\"x\",1)); "
        " return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "1\n");
}

// m2 with `C: B<int>` declared BEFORE `B<T>`, so `B` is the last implementer in symbol
// order and an order-dependent default would run a different body.
BEZEL_TEST(codegen_iface_generic_dispatch_6307, m2_plain_base_receiver_override_first_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class C: B<int> public constructor() end constructor; public override method Q<V>(e: V, t: int): int => 2; end C; "
        "class B<T>: I<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class P public static method Main(): int "
        " using var r2: I<int> := new B<int>(); "
        " Console.WriteLine(r2.Q<string>(\"x\",1)); "
        " return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "1\n");
}

// A generic implementer whose instantiation is built only inside generic code
// (`new B2<X, int>()` in `F<X>`), beside one the program names. It is the only class
// that can reach the dispatch's default, which runs its own body. "1\n1\n" before and
// after the fix.
BEZEL_TEST(codegen_iface_generic_dispatch_6307, open_generic_family_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class B2<S, T>: I<T> public constructor() end constructor; public method Q<U>(e: U, t: T): int => 1; end B2; "
        "class F<X> public constructor() end constructor; public method Make(): owned I<int> => new B2<X, int>(); end F; "
        "class P public static method Main(): int var f := new F<bool>(); "
        " using var a := f.Make(); using var b: I<int> := new B2<string, int>(); "
        " Console.WriteLine(a.Q<string>(\"x\", 1)); Console.WriteLine(b.Q<string>(\"x\", 1)); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "1\n1\n");
}

// The same open family beside a NON-generic implementer `Cx` declared after it: the
// default used to be the last implementer (`Cx`), so `a` ran `Cx.Q`. Pre-fix "9\n".
BEZEL_TEST(codegen_iface_generic_dispatch_6307, open_generic_family_beside_non_generic_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class B2<S, T>: I<T> public constructor() end constructor; public method Q<U>(e: U, t: T): int => 1; end B2; "
        "class Cx: I<int> public constructor() end constructor; public method Q<U>(e: U, t: int): int => 9; end Cx; "
        "class F<X> public constructor() end constructor; public method Make(): owned I<int> => new B2<X, int>(); end F; "
        "class P public static method Main(): int var f := new F<bool>(); "
        " using var a := f.Make(); "
        " Console.WriteLine(a.Q<string>(\"x\", 1)); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "1\n");
}

// Review of 6c4da456 (s01): two generic families `B1<S,T>`, `B2<S,T>` built only
// inside `F<X>.Make` at a parameter `I<int>` does not pin, and only the LAST-declared
// family is built at runtime. The dispatch cannot enumerate either instantiation;
// its default keeps the older shape's last-implementer choice, which is `B2`'s body
// here. 6c4da456 aborted ("...could not enumerate (#6307)", rc 134) on this program,
// which printed "2\n2\n" before #6307.
BEZEL_TEST(codegen_iface_generic_dispatch_6307, two_open_families_last_built_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class B1<S,T>: I<T> public constructor() end constructor; public method Q<U>(e: U, t: T): int => 1; end B1; "
        "class B2<S,T>: I<T> public constructor() end constructor; public method Q<U>(e: U, t: T): int => 2; end B2; "
        "class F<X> public constructor() end constructor; public method Make(k: int): owned I<int> "
        " if k = 0 then return new B1<X,int>(); end if; return new B2<X,int>(); end Make; end F; "
        "class P public static method Main(): int var f := new F<bool>(); "
        " using var a := f.Make(1); using var b := f.Make(1); "
        " Console.WriteLine(a.Q<string>(\"x\", 1)); Console.WriteLine(b.Q<string>(\"x\", 1)); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n");
}

// Review of 6c4da456 (s03): the same shape with the instantiations built in
// `G<Z>.Mk` / `G<Z>.Mk1` at the caller's own parameter, only `Mk` (the last-declared
// family `B2`) used. "2\n" before #6307; 6c4da456 aborted.
BEZEL_TEST(codegen_iface_generic_dispatch_6307, two_open_families_generic_factory_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class B1<S,T>: I<T> public constructor() end constructor; public method Q<U>(e: U, t: T): int => 1; end B1; "
        "class B2<S,T>: I<T> public constructor() end constructor; public method Q<U>(e: U, t: T): int => 2; end B2; "
        "class G<Z> public constructor() end constructor; public method Mk(): owned I<Z> return new B2<bool,Z>(); end Mk; "
        " public method Mk1(): owned I<Z> return new B1<bool,Z>(); end Mk1; end G; "
        "class P public static method Main(): int var g := new G<int>(); "
        " using var a := g.Mk(); "
        " Console.WriteLine(a.Q<string>(\"x\", 1)); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n");
}
