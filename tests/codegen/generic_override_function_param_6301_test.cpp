// Codegen -- §12.1 (v0.338): an `override` replaces the base's virtual slot.
// Issue #6301: a function-typed parameter of the overridden generic method
// (`Q<U>(e: U, f: (T) -> U)` overridden as `Q<V>(e: V, f: (int) -> V)`) made the
// override-by-signature comparison fail, so a base-typed receiver ran the base body.
//
// Pre-fix (src/codegen/LlvmBackend_impl.hpp, Emitter::methodSigParamsMatchUnder) a
// function type was compared with no substitution through its parameter and return
// types. It is now compared structurally through the chain frames.
// Each row's observable is captured stdout. The rows use runAot, not runAotChecked:
// a closure passed to a VIRTUAL generic method leaks its cell today (#6306), which the
// checked oracle would report on the fixed output too.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// The issue repro (q06). Pre-fix "1\n".
BEZEL_TEST(codegen_generic_override_function_param_6301, function_param_override_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "class B<T> public constructor() end constructor; public virtual method Q<U>(e: U, f: (T) -> U): int => 1; end B; "
        "class D: B<int> public constructor() end constructor; public override method Q<V>(e: V, f: (int) -> V): int => 3; end D; "
        "class P public static method Main(): int var d: B<int> := new D(); var f: (int) -> string := lambda(i: int) => \"s\"; "
        " Console.WriteLine(d.Q<string>(\"x\",f)); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "3\n");
}

// `D<X>: B<X>` with `f: (X) -> V`. Pre-fix "1\n".
BEZEL_TEST(codegen_generic_override_function_param_6301, function_param_generic_derived_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "class B<T> public constructor() end constructor; public virtual method Q<U>(e: U, f: (T) -> U): int => 1; end B; "
        "class D<X>: B<X> public constructor() end constructor; public override method Q<V>(e: V, f: (X) -> V): int => 3; end D; "
        "class P public static method Main(): int var d: B<int> := new D<int>(); var f: (int) -> string := lambda(i: int) => \"s\"; "
        " Console.WriteLine(d.Q<string>(\"x\",f)); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "3\n");
}

// `C: B<string>` and a plain `B<int>` receiver. Pre-fix "1\n1\n".
BEZEL_TEST(codegen_generic_override_function_param_6301, function_param_string_and_plain_base_12_1_v0338) {
    auto r = runAot(
        "module M; "
        "class B<T> public constructor() end constructor; public virtual method Q<U>(e: U, f: (T) -> U): int => 1; end B; "
        "class C: B<string> public constructor() end constructor; public override method Q<V>(e: V, f: (string) -> V): int => 3; end C; "
        "class P public static method Main(): int var d: B<string> := new C(); var p: B<int> := new B<int>(); var f: (string) -> int := lambda(s: string) => 1; var g: (int) -> int := lambda(s: int) => 1; "
        " Console.WriteLine(d.Q<int>(1,f)); Console.WriteLine(p.Q<int>(1,g)); return 0; end Main; end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "3\n1\n");
}

