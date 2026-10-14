// Codegen -- §12.1 (v0.338): an `override` replaces the base's virtual slot, so a
// call through the BASE type reaches the override. Issue #6301: `D<X>: B<Array<X>>`
// overriding the generic method `B<T>.Q<U>(e: U, t: T)` as
// `Q<V>(e: V, t: Array<X>)` ran the base body through a base-typed receiver.
//
// Pre-fix (src/codegen/LlvmBackend_Calls2.cpp, deriveDescendantGenericSubst) a
// base-clause argument that is a generic type over the level's parameters could not
// be inverted, so the candidate made the whole dispatch unenumerable and the
// receiver ran the erased vtable template. It now matches the argument's shape
// against the receiver's static type (`Array<int>` pins X = int), and a shape that
// can never equal it removes the candidate. Each row's observable is captured stdout.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// The issue repro (q03). Pre-fix stdout "1\n".
BEZEL_TEST(codegen_generic_override_nested_base_arg_6301, nested_array_arg_override_12_1_v0338) {
    auto r = runAotChecked(
        "module M; "
        "class B<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class D<X>: B<Array<X>> public constructor() end constructor; public override method Q<V>(e: V, t: Array<X>): int => 3; end D; "
        "class P public static method Main(): int var d: B<Array<int>> := new D<int>(); var a: Array<int> := [1]; "
        " Console.WriteLine(d.Q<int>(1,a)); a.Dispose(); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "3\n");
}

// The issue repro (q08): `C<X>: B<X>` alone was right; declaring `D<X>: B<Array<X>>` broke it. Pre-fix "1\n".
BEZEL_TEST(codegen_generic_override_nested_base_arg_6301, unrelated_sibling_declaration_12_1_v0338) {
    auto r = runAotChecked(
        "module M; "
        "class B<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class C<X>: B<X> public constructor() end constructor; public override method Q<V>(e: V, t: X): int => 2; end C; "
        "class D<X>: B<Array<X>> public constructor() end constructor; public override method Q<V>(e: V, t: Array<X>): int => 3; end D; "
        "class P public static method Main(): int var b: B<string> := new C<string>(); "
        " Console.WriteLine(b.Q<int>(1,\"s\")); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n");
}

// `F: D<int>` overriding again, and a plain `D<int>` receiver. Pre-fix "1\n1\n".
BEZEL_TEST(codegen_generic_override_nested_base_arg_6301, two_instantiations_and_derived_chain_12_1_v0338) {
    auto r = runAotChecked(
        "module M; "
        "class B<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class D<X>: B<Array<X>> public constructor() end constructor; public override method Q<V>(e: V, t: Array<X>): int => 3; end D; "
        "class F: D<int> public constructor() end constructor; public override method Q<V>(e: V, t: Array<int>): int => 6; end F; "
        "class P public static method Main(): int var d: B<Array<int>> := new F(); var g: B<Array<int>> := new D<int>(); var a: Array<int> := [1]; "
        " Console.WriteLine(d.Q<int>(1,a)); Console.WriteLine(g.Q<int>(1,a)); a.Dispose(); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "6\n3\n");
}

// D<int>, a non-generic `E: B<Array<int>>` and a plain base. Pre-fix "1\n1\n1\n".
BEZEL_TEST(codegen_generic_override_nested_base_arg_6301, sibling_overrides_and_base_receivers_12_1_v0338) {
    auto r = runAotChecked(
        "module M; "
        "class B<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class D<X>: B<Array<X>> public constructor() end constructor; public override method Q<V>(e: V, t: Array<X>): int => 3; end D; "
        "class E: B<Array<int>> public constructor() end constructor; public override method Q<V>(e: V, t: Array<int>): int => 4; end E; "
        "class P public static method Main(): int var d: B<Array<int>> := new D<int>(); var e: B<Array<int>> := new E(); var b: B<Array<int>> := new B<Array<int>>(); "
        " var a: Array<int> := [1]; "
        " Console.WriteLine(d.Q<int>(1,a)); Console.WriteLine(e.Q<int>(1,a)); Console.WriteLine(b.Q<int>(1,a)); a.Dispose(); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "3\n4\n1\n");
}

// `C<X>: B<X>` beside `D<X>: B<Array<X>>`, both instantiated. Pre-fix "1\n1\n".
BEZEL_TEST(codegen_generic_override_nested_base_arg_6301, generic_sibling_beside_nested_12_1_v0338) {
    auto r = runAotChecked(
        "module M; "
        "class B<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class C<X>: B<X> public constructor() end constructor; public override method Q<V>(e: V, t: X): int => 2; end C; "
        "class D<X>: B<Array<X>> public constructor() end constructor; public override method Q<V>(e: V, t: Array<X>): int => 3; end D; "
        "class P public static method Main(): int var b: B<string> := new C<string>(); var d: B<Array<int>> := new D<int>(); var a: Array<int> := [1]; "
        " Console.WriteLine(b.Q<int>(1,\"s\")); Console.WriteLine(d.Q<int>(1,a)); a.Dispose(); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n3\n");
}

// D<int> and D<string> receivers. Pre-fix "1\n1\n".
BEZEL_TEST(codegen_generic_override_nested_base_arg_6301, both_element_types_12_1_v0338) {
    auto r = runAotChecked(
        "module M; "
        "class B<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class D<X>: B<Array<X>> public constructor() end constructor; public override method Q<V>(e: V, t: Array<X>): int => 3; end D; "
        "class P public static method Main(): int var d: B<Array<int>> := new D<int>(); var s: B<Array<string>> := new D<string>(); "
        " var a: Array<int> := [1]; var c: Array<string> := [\"z\"]; "
        " Console.WriteLine(d.Q<int>(1,a)); Console.WriteLine(s.Q<string>(\"x\",c)); a.Dispose(); c.Dispose(); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "3\n3\n");
}

// A user generic `Box<X>` argument, and a plain `B<Box<string>>`. Pre-fix "1\n1\n".
BEZEL_TEST(codegen_generic_override_nested_base_arg_6301, inherits_and_user_generic_arg_12_1_v0338) {
    auto r = runAotChecked(
        "module M; "
        "class Box<X> public constructor() end constructor; end Box; "
        "class B<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class D<X>: B<Box<X>> public constructor() end constructor; public override method Q<V>(e: V, t: Box<X>): int => 3; end D; "
        "class P public static method Main(): int var d: B<Box<int>> := new D<int>(); var s: B<Box<string>> := new B<Box<string>>(); var bx: Box<int> := new Box<int>(); var bs: Box<string> := new Box<string>(); "
        " Console.WriteLine(d.Q<int>(1,bx)); Console.WriteLine(s.Q<int>(1,bs)); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "3\n1\n");
}

