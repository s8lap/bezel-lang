// Codegen -- §12.1 / §11.2 (v0.338): a call through an interface reaches the
// override. Issue #6301: `B<T>: I<T>` implementing `I<T>.Q<U>(e: U, t: T)` by a
// virtual method, overridden in `C: B<int>`, ran the base body through an `I<int>`
// receiver.
//
// Pre-fix (Emitter::findMethodBySignature) the interface's `T` was compared with
// the override's `int` unsubstituted, so the walk stopped at the base's own method.
// The interface method is now read through the level that names the interface.
// A generic implementer and a twice-instantiated interface stay open (#6307).
// Each row's observable is captured stdout.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// The issue repro (p16). Pre-fix "2\n1\n2\n".
BEZEL_TEST(codegen_generic_override_interface_route_6301, interface_receiver_reaches_override_12_1_v0338) {
    auto r = runAotChecked(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class B<T>: I<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class C: B<int> public constructor() end constructor; public override method Q<V>(e: V, t: int): int => 2; end C; "
        "class P public static method Main(): int var c: C := new C(); var b: B<int> := c; var i: I<int> := c; "
        " Console.WriteLine(b.Q<string>(\"x\",1)); Console.WriteLine(i.Q<string>(\"x\",1)); Console.WriteLine(c.Q<string>(\"x\",1)); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n");
}

// `C: B<int>` overriding, `D: B<string>` inheriting, and a plain `B<int>`. Pre-fix "1\n1\n1\n".
BEZEL_TEST(codegen_generic_override_interface_route_6301, interface_receivers_per_instantiation_12_1_v0338) {
    auto r = runAotChecked(
        "module M; "
        "interface I<T> method Q<U>(e: U, t: T): int; end I; "
        "class B<T>: I<T> public constructor() end constructor; public virtual method Q<U>(e: U, t: T): int => 1; end B; "
        "class C: B<int> public constructor() end constructor; public override method Q<V>(e: V, t: int): int => 2; end C; "
        "class D: B<string> public constructor() end constructor; end D; "
        "class P public static method Main(): int var c: I<int> := new C(); var d: I<string> := new D(); var b: I<int> := new B<int>(); "
        " Console.WriteLine(c.Q<string>(\"x\",1)); Console.WriteLine(d.Q<int>(1,\"s\")); Console.WriteLine(b.Q<string>(\"x\",1)); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n1\n1\n");
}

