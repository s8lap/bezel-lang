// #6996 (§14.1, v0.340): "An instantiation of a generic declaration is the
// constructed type or constructed member obtained by substituting type
// arguments for its type parameters". A BARE (implicit-`this`) call of a
// method inherited from a constructed generic base, `Get()` in
// `class Sub: Box<string>`, must be typed with the base's substitution
// (T := string), exactly as `this.Get()` is. Before the fix the call stayed
// typed as the bare `T`, `.Length` failed to lower and the method silently
// returned 0 (#2491 warning). Every row asserts exact stdout.
#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAot;

namespace {
void expectOut(const std::string& program, const std::string& want) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}
void expectMods(const std::string& app, const std::string& want) {
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", R"BZ(
module Lib;
public class Box<T>
  public V: T;
  public constructor(v: T) V := v; end constructor;
  public method Get(): T => V;
  public property P: T => V;
end Box;
public record Pt(X: int, Y: int);
public class Item
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Item;
)BZ"});
    mods.push_back({"App", app});
    JitResult r = bezel::test::runAotModules(mods, "App");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}
}  // namespace

BEZEL_TEST(inherited_generic_method_result_member_6996, bare_call_member_expr_body) {
    expectOut(R"BZ(module M;
public class Box<T>
  public V: T;
  public constructor(v: T) V := v; end constructor;
  public method Get(): T => V;
  public property P: T => V;
  public static method Mk(v: T): T => v;
  public method Pick<U>(u: U): U => u;
  public method Own(): T => Get();
end Box;
public record Pt(X: int, Y: int);
public class Item
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Item;

class Sub: Box<string>
  public constructor(v: string) base(v); end constructor;
  public method L(): int => Get().Length;
end Sub;
class C public static method Main(): int using var s := new Sub("sub1234"); Console.WriteLine(s.L()); return 0; end Main; end C;
)BZ",
              "7\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, bare_call_member_block_body) {
    expectOut(R"BZ(module M;
public class Box<T>
  public V: T;
  public constructor(v: T) V := v; end constructor;
  public method Get(): T => V;
  public property P: T => V;
  public static method Mk(v: T): T => v;
  public method Pick<U>(u: U): U => u;
  public method Own(): T => Get();
end Box;
public record Pt(X: int, Y: int);
public class Item
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Item;

class Sub: Box<string>
  public constructor(v: string) base(v); end constructor;
  public method L(): int
    return Get().Length;
  end L;
end Sub;
class C public static method Main(): int using var s := new Sub("sub1234"); Console.WriteLine(s.L()); return 0; end Main; end C;
)BZ",
              "7\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, this_call_member_control) {
    expectOut(R"BZ(module M;
public class Box<T>
  public V: T;
  public constructor(v: T) V := v; end constructor;
  public method Get(): T => V;
  public property P: T => V;
  public static method Mk(v: T): T => v;
  public method Pick<U>(u: U): U => u;
  public method Own(): T => Get();
end Box;
public record Pt(X: int, Y: int);
public class Item
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Item;

class Sub: Box<string>
  public constructor(v: string) base(v); end constructor;
  public method L(): int => this.Get().Length;
end Sub;
class C public static method Main(): int using var s := new Sub("sub1234"); Console.WriteLine(s.L()); return 0; end Main; end C;
)BZ",
              "7\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, base_call_member_control) {
    expectOut(R"BZ(module M;
public class Box<T>
  public V: T;
  public constructor(v: T) V := v; end constructor;
  public method Get(): T => V;
  public property P: T => V;
  public static method Mk(v: T): T => v;
  public method Pick<U>(u: U): U => u;
  public method Own(): T => Get();
end Box;
public record Pt(X: int, Y: int);
public class Item
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Item;

class Sub: Box<string>
  public constructor(v: string) base(v); end constructor;
  public method L(): int => base.Get().Length;
end Sub;
class C public static method Main(): int using var s := new Sub("sub1234"); Console.WriteLine(s.L()); return 0; end Main; end C;
)BZ",
              "7\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, bare_call_int_arithmetic) {
    expectOut(R"BZ(module M;
public class Box<T>
  public V: T;
  public constructor(v: T) V := v; end constructor;
  public method Get(): T => V;
  public property P: T => V;
  public static method Mk(v: T): T => v;
  public method Pick<U>(u: U): U => u;
  public method Own(): T => Get();
end Box;
public record Pt(X: int, Y: int);
public class Item
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Item;

class Sub: Box<int>
  public constructor(v: int) base(v); end constructor;
  public method L(): int => Get() + 1;
end Sub;
class C public static method Main(): int using var s := new Sub(41); Console.WriteLine(s.L()); return 0; end Main; end C;
)BZ",
              "42\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, bare_call_class_t_member) {
    expectOut(R"BZ(module M;
public class Box<T>
  public V: T;
  public constructor(v: T) V := v; end constructor;
  public method Get(): T => V;
  public property P: T => V;
  public static method Mk(v: T): T => v;
  public method Pick<U>(u: U): U => u;
  public method Own(): T => Get();
end Box;
public record Pt(X: int, Y: int);
public class Item
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Item;

class Sub: Box<Item>
  public constructor(v: Item) base(v); end constructor;
  public method L(): int => Get().N;
end Sub;
class C public static method Main(): int var it := new Item(9); using var s := new Sub(it); Console.WriteLine(s.L()); return 0; end Main; end C;
)BZ",
              "9\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, bare_call_record_t_fields) {
    expectOut(R"BZ(module M;
public class Box<T>
  public V: T;
  public constructor(v: T) V := v; end constructor;
  public method Get(): T => V;
  public property P: T => V;
  public static method Mk(v: T): T => v;
  public method Pick<U>(u: U): U => u;
  public method Own(): T => Get();
end Box;
public record Pt(X: int, Y: int);
public class Item
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Item;

class Sub: Box<Pt>
  public constructor(v: Pt) base(v); end constructor;
  public method L(): int => Get().X + Get().Y;
end Sub;
class C public static method Main(): int using var s := new Sub(new Pt(3, 4)); Console.WriteLine(s.L()); return 0; end Main; end C;
)BZ",
              "7\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, bare_call_two_level_chain) {
    expectOut(R"BZ(module M;
public class Box<T>
  public V: T;
  public constructor(v: T) V := v; end constructor;
  public method Get(): T => V;
  public property P: T => V;
  public static method Mk(v: T): T => v;
  public method Pick<U>(u: U): U => u;
  public method Own(): T => Get();
end Box;
public record Pt(X: int, Y: int);
public class Item
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Item;

class Sub: Box<string>
  public constructor(v: string) base(v); end constructor;
end Sub;
class Sub2: Sub
  public constructor(v: string) base(v); end constructor;
  public method L(): int => Get().Length;
end Sub2;
class C public static method Main(): int using var s := new Sub2("sub1234"); Console.WriteLine(s.L()); return 0; end Main; end C;
)BZ",
              "7\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, bare_call_generic_derived_instantiated) {
    expectOut(R"BZ(module M;
public class Box<T>
  public V: T;
  public constructor(v: T) V := v; end constructor;
  public method Get(): T => V;
  public property P: T => V;
  public static method Mk(v: T): T => v;
  public method Pick<U>(u: U): U => u;
  public method Own(): T => Get();
end Box;
public record Pt(X: int, Y: int);
public class Item
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Item;

class Sub<U>: Box<U>
  public constructor(v: U) base(v); end constructor;
end Sub;
class Sub3: Sub<string>
  public constructor(v: string) base(v); end constructor;
  public method L(): int => Get().Length;
end Sub3;
class C public static method Main(): int using var s := new Sub3("sub1234"); Console.WriteLine(s.L()); return 0; end Main; end C;
)BZ",
              "7\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, bare_property_member_control) {
    expectOut(R"BZ(module M;
public class Box<T>
  public V: T;
  public constructor(v: T) V := v; end constructor;
  public method Get(): T => V;
  public property P: T => V;
  public static method Mk(v: T): T => v;
  public method Pick<U>(u: U): U => u;
  public method Own(): T => Get();
end Box;
public record Pt(X: int, Y: int);
public class Item
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Item;

class Sub: Box<string>
  public constructor(v: string) base(v); end constructor;
  public method L(): int => P.Length;
end Sub;
class C public static method Main(): int using var s := new Sub("sub1234"); Console.WriteLine(s.L()); return 0; end Main; end C;
)BZ",
              "7\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, bare_field_member_control) {
    expectOut(R"BZ(module M;
public class Box<T>
  public V: T;
  public constructor(v: T) V := v; end constructor;
  public method Get(): T => V;
  public property P: T => V;
  public static method Mk(v: T): T => v;
  public method Pick<U>(u: U): U => u;
  public method Own(): T => Get();
end Box;
public record Pt(X: int, Y: int);
public class Item
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Item;

class Sub: Box<string>
  public constructor(v: string) base(v); end constructor;
  public method L(): int => V.Length;
end Sub;
class C public static method Main(): int using var s := new Sub("sub1234"); Console.WriteLine(s.L()); return 0; end Main; end C;
)BZ",
              "7\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, bare_call_toupper_indexer_local) {
    expectOut(R"BZ(module M;
public class Box<T>
  public V: T;
  public constructor(v: T) V := v; end constructor;
  public method Get(): T => V;
  public property P: T => V;
  public static method Mk(v: T): T => v;
  public method Pick<U>(u: U): U => u;
  public method Own(): T => Get();
end Box;
public record Pt(X: int, Y: int);
public class Item
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Item;

class Sub: Box<string>
  public constructor(v: string) base(v); end constructor;
  public method U(): string => Get().ToUpper();
  public method I(): char => Get()[1];
  public method S(): string => Get();
  public method L(): int
    var x := Get();
    return x.Length;
  end L;
end Sub;
class C public static method Main(): int using var s := new Sub("sub1234"); Console.WriteLine(s.U()); Console.WriteLine(s.I()); Console.WriteLine(s.S()); Console.WriteLine(s.L()); return 0; end Main; end C;
)BZ",
              "SUB1234\nu\nsub1234\n7\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, non_generic_base_control) {
    expectOut(R"BZ(module M;
public class Box<T>
  public V: T;
  public constructor(v: T) V := v; end constructor;
  public method Get(): T => V;
  public property P: T => V;
  public static method Mk(v: T): T => v;
  public method Pick<U>(u: U): U => u;
  public method Own(): T => Get();
end Box;
public record Pt(X: int, Y: int);
public class Item
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Item;

public class NB
  public method Name(): string => "hello";
end NB;
class Sub: NB
  public method L(): int => Name().Length;
end Sub;
class C public static method Main(): int var s := new Sub(); Console.WriteLine(s.L()); return 0; end Main; end C;
)BZ",
              "5\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, own_generic_class_bare_call_control) {
    expectOut(R"BZ(module M;
public class Box<T>
  public V: T;
  public constructor(v: T) V := v; end constructor;
  public method Get(): T => V;
  public property P: T => V;
  public static method Mk(v: T): T => v;
  public method Pick<U>(u: U): U => u;
  public method Own(): T => Get();
end Box;
public record Pt(X: int, Y: int);
public class Item
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Item;

class C public static method Main(): int using var b := new Box<string>("abcd"); Console.WriteLine(b.Own().Length); return 0; end Main; end C;
)BZ",
              "4\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, bare_call_with_method_type_param) {
    expectOut(R"BZ(module M;
public class Box<T>
  public V: T;
  public constructor(v: T) V := v; end constructor;
  public method Get(): T => V;
  public property P: T => V;
  public static method Mk(v: T): T => v;
  public method Pick<U>(u: U): U => u;
  public method Own(): T => Get();
end Box;
public record Pt(X: int, Y: int);
public class Item
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Item;

class Sub: Box<string>
  public constructor(v: string) base(v); end constructor;
  public method L(): int => Pick<int>(5) + Own().Length;
end Sub;
class C public static method Main(): int using var s := new Sub("sub1234"); Console.WriteLine(s.L()); return 0; end Main; end C;
)BZ",
              "12\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, bare_call_repeated_chain) {
    expectOut(R"BZ(module M;
public class Box<T>
  public V: T;
  public constructor(v: T) V := v; end constructor;
  public method Get(): T => V;
  public property P: T => V;
  public static method Mk(v: T): T => v;
  public method Pick<U>(u: U): U => u;
  public method Own(): T => Get();
end Box;
public record Pt(X: int, Y: int);
public class Item
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Item;

class Sub: Box<string>
  public constructor(v: string) base(v); end constructor;
  public method L(): int => Get().Length + Get().Length;
end Sub;
class Sub2: Sub
  public constructor(v: string) base(v); end constructor;
  public method M(): int => L() + Get().Length;
end Sub2;
class C public static method Main(): int using var s := new Sub2("abc"); Console.WriteLine(s.M()); return 0; end Main; end C;
)BZ",
              "9\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, imported_bare_call_member) {
    expectMods(R"BZ(module App;
import Lib;
class Sub: Box<string>
  public constructor(v: string) base(v); end constructor;
  public method L(): int => Get().Length;
end Sub;
class C public static method Main(): int using var s := new Sub("sub1234"); Console.WriteLine(s.L()); return 0; end Main; end C;
)BZ",
               "7\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, imported_bare_call_int) {
    expectMods(R"BZ(module App;
import Lib;
class Sub: Box<int>
  public constructor(v: int) base(v); end constructor;
  public method L(): int => Get() + 1;
end Sub;
class C public static method Main(): int using var s := new Sub(41); Console.WriteLine(s.L()); return 0; end Main; end C;
)BZ",
               "42\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, imported_bare_call_record) {
    expectMods(R"BZ(module App;
import Lib;
class Sub: Box<Pt>
  public constructor(v: Pt) base(v); end constructor;
  public method L(): int => Get().X + Get().Y;
end Sub;
class C public static method Main(): int using var s := new Sub(new Pt(3, 4)); Console.WriteLine(s.L()); return 0; end Main; end C;
)BZ",
               "7\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, imported_two_level_chain) {
    expectMods(R"BZ(module App;
import Lib;
class Sub: Box<string>
  public constructor(v: string) base(v); end constructor;
end Sub;
class Sub2: Sub
  public constructor(v: string) base(v); end constructor;
  public method L(): int => Get().Length;
  public method U(): string => Get().ToUpper();
end Sub2;
class C public static method Main(): int using var s := new Sub2("sub1234"); Console.WriteLine(s.L()); Console.WriteLine(s.U()); return 0; end Main; end C;
)BZ",
               "7\nSUB1234\n");
}

BEZEL_TEST(inherited_generic_method_result_member_6996, imported_this_base_property_controls) {
    expectMods(R"BZ(module App;
import Lib;
class Sub: Box<string>
  public constructor(v: string) base(v); end constructor;
  public method L(): int => this.Get().Length;
  public method B(): int => base.Get().Length;
  public method Q(): int => P.Length;
end Sub;
class C public static method Main(): int using var s := new Sub("sub1234"); Console.WriteLine(s.L()); Console.WriteLine(s.B()); Console.WriteLine(s.Q()); return 0; end Main; end C;
)BZ",
               "7\n7\n7\n");
}

