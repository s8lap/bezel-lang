// Codegen — §12.1 (v0.338): an `override` replaces the base's virtual slot, so
// a call through the BASE type reaches the override. Issue #6298: a generic
// METHOD overridden in a derived class of a GENERIC base, where the signature
// also mentions a base class type parameter (`Q<U>(e: U, t: T)` overridden as
// `Q<V>(e: V, t: int)` in `C: B<int>`), dispatched the base body through a
// base-typed receiver (the derived-typed call was right).
//
// Pre-fix (src/codegen/LlvmBackend_impl.hpp, Emitter::findMethodBySignature)
// the override's `t: int` was compared with the base's unsubstituted `t: T`,
// never matched, and the walk up the chain reached the base's own method, an
// exact self-match: the base body ran. The comparison now reads both sides
// through the chain substitution into the receiver class's scope.
//
// Each row's observable is captured stdout; pre-fix values are in the comments.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

const char* kHead =
    "module M; "
    "class B<T> public constructor() end constructor; "
    "  public virtual method Q<U>(e: U, t: T): int => 1; end B; ";

}  // namespace

// The issue's repro. Pre-fix stdout "2\n1\n2\n1\n".
BEZEL_TEST(codegen_generic_method_override_generic_base_6298,
           override_through_base_type_12_1_v0338) {
    auto r = runAotChecked(
        std::string(kHead) +
        "class C: B<int> public constructor() end constructor; "
        "  public override method Q<V>(e: V, t: int): int => 2; end C; "
        "class P public static method Main(): int "
        "  var c: C := new C(); var b: B<int> := c; "
        "  Console.WriteLine(c.Q<string>(\"x\", 1)); "
        "  Console.WriteLine(b.Q<string>(\"x\", 1)); "
        "  Console.WriteLine(c.Q<int>(5, 1)); "
        "  Console.WriteLine(b.Q<int>(5, 1)); "
        "  return 0; end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n2\n");
}

// Multi-level chain `E: D<int>`, `D<U>: B<U>`, each level overriding. Pre-fix
// stdout "1\n1\n1\n".
BEZEL_TEST(codegen_generic_method_override_generic_base_6298,
           multi_level_generic_chain_12_1_v0338) {
    auto r = runAotChecked(
        std::string(kHead) +
        "class D<U>: B<U> public constructor() end constructor; "
        "  public override method Q<X>(e: X, t: U): int => 2; end D; "
        "class E: D<int> public constructor() end constructor; "
        "  public override method Q<Y>(e: Y, t: int): int => 3; end E; "
        "class P public static method Main(): int "
        "  var d: B<int> := new D<int>(); var e: B<int> := new E(); "
        "  var s: B<string> := new D<string>(); "
        "  Console.WriteLine(d.Q<string>(\"x\", 1)); "
        "  Console.WriteLine(e.Q<string>(\"x\", 1)); "
        "  Console.WriteLine(s.Q<int>(4, \"q\")); "
        "  return 0; end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n3\n2\n");
}

// A class parameter and an own method parameter that share the name `U`
// (`G<U>.Q<T>` overridden as `Q<U>`), and a generic derived class forwarding
// its parameter (`C<X>: B2<X>`). Pre-fix stdout "2\n1\n" (the second row ran
// the base body).
BEZEL_TEST(codegen_generic_method_override_generic_base_6298,
           generic_derived_and_name_collision_12_1_v0338) {
    auto r = runAotChecked(
        "module M; "
        "class G<U> public constructor() end constructor; "
        "  public virtual method Q<T>(e: T, u: U): int => 1; end G; "
        "class H: G<int> public constructor() end constructor; "
        "  public override method Q<U>(e: U, t: int): int => 2; end H; "
        "class C<X>: B2<X> public constructor() end constructor; "
        "  public override method Q<V>(e: V, t: X): int => 3; end C; "
        "class B2<T> public constructor() end constructor; "
        "  public virtual method Q<U>(e: U, t: T): int => 1; end B2; "
        "class P public static method Main(): int "
        "  var g: G<int> := new H(); "
        "  var b: B2<string> := new C<string>(); "
        "  Console.WriteLine(g.Q<string>(\"x\", 1)); "
        "  Console.WriteLine(b.Q<int>(7, \"s\")); "
        "  return 0; end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n3\n");
}

// Overloads: only the matching one is overridden; the overload with a
// different arity keeps the base body, and a sibling class that does not
// override and a base instance both still run the base body. Pre-fix stdout
// "1\n10\n1\n1\n1\n".
BEZEL_TEST(codegen_generic_method_override_generic_base_6298,
           only_the_matching_overload_and_non_overriders_12_1_v0338) {
    auto r = runAotChecked(
        "module M; "
        "class B<T> public constructor() end constructor; "
        "  public virtual method Q<U>(e: U, t: T): int => 1; "
        "  public virtual method Q<U>(t: T, e: U, z: int): int => 10; end B; "
        "class C: B<int> public constructor() end constructor; "
        "  public override method Q<V>(e: V, t: int): int => 2; end C; "
        "class E: C public constructor() end constructor; end E; "
        "class F: B<int> public constructor() end constructor; end F; "
        "class P public static method Main(): int "
        "  var c: B<int> := new C(); var e: B<int> := new E(); "
        "  var f: B<int> := new F(); var bb: B<int> := new B<int>(); "
        "  Console.WriteLine(c.Q<string>(\"x\", 1)); "
        "  Console.WriteLine(c.Q<string>(1, \"x\", 0)); "
        "  Console.WriteLine(e.Q<string>(\"x\", 1)); "
        "  Console.WriteLine(f.Q<string>(\"x\", 1)); "
        "  Console.WriteLine(bb.Q<string>(\"x\", 1)); "
        "  return 0; end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n10\n2\n1\n1\n");
}
