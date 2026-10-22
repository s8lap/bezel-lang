// Spec-derived regression test for issue #7003.
//
// Spec §19.5 (v0.340): "A typed binding pattern `var n: T` also matches only non-null values — the type annotation makes it a type-constrained binding, not an unconditional binding." §19.3 (v0.340): a type pattern matches when the value is of that type.
//
// A typed binding `var c: T` in a destructuring slot (positional, named, boxed record, choice payload) is a runtime TYPE test, as the top-level `case x of var c: T` is: a Sq in a Shape slot does not bind `var c: Circle`.
//
// Rows are exact-stdout. A row uses `runAotChecked` (sanitizers armed, leak
// oracle on) where its program leaks nothing, and `runAot` where the program
// builds a fresh `new` argument or a boxed `Object?` that the compiler does not
// reclaim (the #7005 box/temporary leak, present on the pre-fix compiler too).
// The probes live in /var/tmp/bezel-scratch/r34-7003/p/.

#include <string>

#include "support/aot_support.hpp"  // runAot, runAotChecked
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string checkedOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}

std::string plainOut(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}

}  // namespace

BEZEL_TEST(codegen_pattern_typed_slot_type_test_7003, issue_repro_class_slot_subtype_and_int_slot) {
    BEZEL_EXPECT_EQ(
        plainOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
public class Sq : Shape end Sq;
public class Big : Circle end Big;
public interface IFoo method Id(): int; end IFoo;
public class FA : IFoo public method Id(): int return 1; end Id; end FA;
public class FB public method Id(): int return 2; end Id; end FB;
record Pt(X: int, Y: int);
record SP(S: Shape?, I: int?);
class C
  public static method T(p: SP): string
    return (case p of SP(var c: Circle, var i) => "circle", _ => "other");
  end T;
  public static method V(p: SP): string
    return (case p of SP(var s, var i: int) => "int" + i.ToString(), _ => "other");
  end V;
  public static method Main(): int
    Console.WriteLine(T(new SP(new Sq(), 5)));
    Console.WriteLine(T(new SP(new Circle(), 5)));
    Console.WriteLine(T(new SP(new Big(), 5)));
    Console.WriteLine(T(new SP(null, 5)));
    Console.WriteLine(V(new SP(null, 5)));
    Console.WriteLine(V(new SP(null, null)));
    Console.WriteLine(V(new SP(null, -77)));
    return 0;
  end Main;
end C;)BZL"),
        std::string("other\ncircle\ncircle\nother\nint5\nother\nint-77\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_type_test_7003, named_slot_type_test) {
    BEZEL_EXPECT_EQ(
        plainOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
public class Sq : Shape end Sq;
public class Big : Circle end Big;
public interface IFoo method Id(): int; end IFoo;
public class FA : IFoo public method Id(): int return 1; end Id; end FA;
public class FB public method Id(): int return 2; end Id; end FB;
record Pt(X: int, Y: int);
record SP(S: Shape?, I: int?);
class C
  public static method T(p: SP): string
    return (case p of SP(S = var c: Circle, I = var i: int) => "circle" + i.ToString(), SP(S = var c: Sq, I = var i) => "sq", _ => "other");
  end T;
  public static method Main(): int
    Console.WriteLine(T(new SP(new Sq(), 5)));
    Console.WriteLine(T(new SP(new Circle(), 5)));
    Console.WriteLine(T(new SP(new Circle(), null)));
    Console.WriteLine(T(new SP(new Big(), 9)));
    Console.WriteLine(T(new SP(null, 5)));
    return 0;
  end Main;
end C;)BZL"),
        std::string("sq\ncircle5\nother\ncircle9\nother\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_type_test_7003, nested_slot_depth_two) {
    BEZEL_EXPECT_EQ(
        plainOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
public class Sq : Shape end Sq;
public class Big : Circle end Big;
public interface IFoo method Id(): int; end IFoo;
public class FA : IFoo public method Id(): int return 1; end Id; end FA;
public class FB public method Id(): int return 2; end Id; end FB;
record Pt(X: int, Y: int);
record In(S: Shape?);
record Out(A: In, N: int);
class C
  public static method T(o: Out): string
    return (case o of Out(In(var c: Circle), var n) => "circle", Out(In(var q: Sq), var n) => "sq", _ => "other");
  end T;
  public static method Main(): int
    Console.WriteLine(T(new Out(new In(new Sq()), 1)));
    Console.WriteLine(T(new Out(new In(new Circle()), 1)));
    Console.WriteLine(T(new Out(new In(new Big()), 1)));
    Console.WriteLine(T(new Out(new In(null), 1)));
    return 0;
  end Main;
end C;)BZL"),
        std::string("sq\ncircle\ncircle\nother\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_type_test_7003, interface_and_object_slots) {
    BEZEL_EXPECT_EQ(
        plainOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
public class Sq : Shape end Sq;
public class Big : Circle end Big;
public interface IFoo method Id(): int; end IFoo;
public class FA : IFoo public method Id(): int return 1; end Id; end FA;
public class FB public method Id(): int return 2; end Id; end FB;
record Pt(X: int, Y: int);
class Bx
  public static method I(v: int): Object? var o: Object? := v; return o; end I;
  public static method L(v: long): Object? var o: Object? := v; return o; end L;
  public static method D(v: double): Object? var o: Object? := v; return o; end D;
  public static method Z(v: bool): Object? var o: Object? := v; return o; end Z;
  public static method P(v: Pt): Object? var o: Object? := v; return o; end P;
end Bx;
record IR(F: IFoo?, O: Object?);
class C
  public static method T(r: IR): string
    return (case r of IR(var f: FA, var o) => "fa", IR(var f: IFoo, var o) => "ifoo", _ => "other");
  end T;
  public static method U(r: IR): string
    return (case r of IR(var f, var o: FB) => "fb", IR(var f, var o: IFoo) => "oifoo", IR(var f, var o: Shape) => "oshape", _ => "other");
  end U;
  public static method Main(): int
    Console.WriteLine(T(new IR(new FA(), null)));
    Console.WriteLine(T(new IR(null, null)));
    Console.WriteLine(U(new IR(null, new FB())));
    Console.WriteLine(U(new IR(null, new FA())));
    Console.WriteLine(U(new IR(null, new Sq())));
    Console.WriteLine(U(new IR(null, "str")));
    Console.WriteLine(U(new IR(null, null)));
    return 0;
  end Main;
end C;)BZL"),
        std::string("fa\nother\nfb\noifoo\noshape\nother\nother\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_type_test_7003, choice_payload_class_slots) {
    BEZEL_EXPECT_EQ(
        plainOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
public class Sq : Shape end Sq;
public class Big : Circle end Big;
public interface IFoo method Id(): int; end IFoo;
public class FA : IFoo public method Id(): int return 1; end Id; end FA;
public class FB public method Id(): int return 2; end Id; end FB;
record Pt(X: int, Y: int);
class Bx
  public static method I(v: int): Object? var o: Object? := v; return o; end I;
  public static method L(v: long): Object? var o: Object? := v; return o; end L;
  public static method D(v: double): Object? var o: Object? := v; return o; end D;
  public static method Z(v: bool): Object? var o: Object? := v; return o; end Z;
  public static method P(v: Pt): Object? var o: Object? := v; return o; end P;
end Bx;
choice Ch Hold(S: Shape?, N: int?), Two(A: Shape, B: Object?), Nope; end Ch;
class C
  public static method T(c: Ch): string
    return (case c of Hold(var x: Circle, var n: int) => "circle" + n.ToString(), Hold(var x: Sq, var n) => "sq", Hold(var x, var n: int) => "hn", Two(var a: Sq, var b: Circle) => "sqc", Two(var a: Circle, var b) => "cb", Two(var a, var b: string) => "str", _ => "other");
  end T;
  public static method Main(): int
    Console.WriteLine(T(Ch.Hold(new Circle(), 4)));
    Console.WriteLine(T(Ch.Hold(new Circle(), null)));
    Console.WriteLine(T(Ch.Hold(new Sq(), null)));
    Console.WriteLine(T(Ch.Hold(null, 8)));
    Console.WriteLine(T(Ch.Hold(null, null)));
    Console.WriteLine(T(Ch.Two(new Sq(), new Circle())));
    Console.WriteLine(T(Ch.Two(new Sq(), new Sq())));
    Console.WriteLine(T(Ch.Two(new Circle(), null)));
    Console.WriteLine(T(Ch.Two(new Big(), "x")));
    Console.WriteLine(T(Ch.Two(new Sq(), "x")));
    Console.WriteLine(T(Ch.Nope));
    return 0;
  end Main;
end C;)BZL"),
        std::string("circle4\nother\nsq\nhn\nother\nsqc\nother\ncb\ncb\nstr\nother\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_type_test_7003, case_statement_form) {
    BEZEL_EXPECT_EQ(
        plainOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
public class Sq : Shape end Sq;
public class Big : Circle end Big;
public interface IFoo method Id(): int; end IFoo;
public class FA : IFoo public method Id(): int return 1; end Id; end FA;
public class FB public method Id(): int return 2; end Id; end FB;
record Pt(X: int, Y: int);
record SP(S: Shape?, I: int?);
choice Ch Hold(S: Shape?, N: int?), Nope; end Ch;
class C
  public static method T(p: SP)
    case p of
      SP(var c: Circle, var i: int) => Console.WriteLine("circle" + i.ToString()),
      SP(var s: Sq, var i) => Console.WriteLine("sq"),
      SP(var s, var i: int) => Console.WriteLine("int" + i.ToString()),
      _ => Console.WriteLine("other")
    end case;
  end T;
  public static method U(c: Ch)
    case c of
      Hold(var s: Circle, var n: int) => Console.WriteLine("hc" + n.ToString()),
      Hold(var s, var n: int) => Console.WriteLine("hn" + n.ToString()),
      _ => Console.WriteLine("other")
    end case;
  end U;
  public static method Main(): int
    T(new SP(new Circle(), 7));
    T(new SP(new Sq(), 7));
    T(new SP(null, 7));
    T(new SP(null, null));
    U(Ch.Hold(new Circle(), 3));
    U(Ch.Hold(new Sq(), 3));
    U(Ch.Hold(new Sq(), null));
    U(Ch.Nope);
    return 0;
  end Main;
end C;)BZL"),
        std::string("circle7\nsq\nint7\nother\nhc3\nhn3\nother\nother\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_type_test_7003, loop_100_class_slots) {
    BEZEL_EXPECT_EQ(
        checkedOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
public class Sq : Shape end Sq;
public class Big : Circle end Big;
public interface IFoo method Id(): int; end IFoo;
public class FA : IFoo public method Id(): int return 1; end Id; end FA;
public class FB public method Id(): int return 2; end Id; end FB;
record Pt(X: int, Y: int);
record SP(S: Shape?, I: int?);
choice Ch Hold(S: Shape?, N: int?), Nope; end Ch;
class C
  public static method T(p: SP): int
    return (case p of SP(var c: Circle, var i) => 1, SP(var s: Sq, var i: int) => 2, SP(var s, var i) => 3, _ => 4);
  end T;
  public static method U(c: Ch): int
    return (case c of Hold(var s: Circle, var n: int) => 10, Hold(var s: Sq, var n) => 20, Hold(var s, var n) => 30, _ => 40);
  end U;
  public static method Mk(i: int): owned Shape?
    if i mod 3 = 0 then return new Circle(); end if;
    if i mod 3 = 1 then return new Sq(); end if;
    return null;
  end Mk;
  public static method Main(): int
    var tot: int := 0; var i: int := 0;
    while i < 100 do
      var s: Shape? := C.Mk(i);
      var n: int? := (if i mod 2 = 0 then i else null);
      tot := tot + T(new SP(s, n)) + U(Ch.Hold(s, n));
      i := i + 1;
    end while;
    Console.WriteLine(tot.ToString());
    return 0;
  end Main;
end C;)BZL"),
        std::string("2546\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_type_test_7003, loop_100_owned_class_slots) {
    BEZEL_EXPECT_EQ(
        checkedOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
public class Sq : Shape end Sq;
public class Big : Circle end Big;
public interface IFoo method Id(): int; end IFoo;
public class FA : IFoo public method Id(): int return 1; end Id; end FA;
public class FB public method Id(): int return 2; end Id; end FB;
record Pt(X: int, Y: int);
class Res
  public method Hi(): string return "hi"; end Hi;
end Res;
class Res2 : Res end Res2;
record Holder(A: Res?, B: Res?, N: int);
class C
  public static method T(h: Holder): int
    return (case h of Holder(var a: Res2, var b: Res, var n) => 3, Holder(var a: Res, var b: Res2, var n) => 2, Holder(var a: Res, var b, var n) => 1, _ => 0);
  end T;
  public static method Mx(i: int): owned Res?
    if i mod 2 = 0 then return null; end if;
    return new Res2();
  end Mx;
  public static method My(i: int): owned Res?
    if i mod 3 = 0 then return null; end if;
    if i mod 3 = 1 then return new Res(); end if;
    return new Res2();
  end My;
  public static method Main(): int
    var i: int := 0; var tot: int := 0;
    while i < 100 do
      var x: Res? := C.Mx(i);
      var y: Res? := C.My(i);
      tot := tot + T(new Holder(x, y, i));
      i := i + 1;
    end while;
    Console.WriteLine(tot.ToString());
    return 0;
  end Main;
end C;)BZL"),
        std::string("116\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_type_test_7003, supertype_binding_still_matches) {
    BEZEL_EXPECT_EQ(
        plainOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
public class Sq : Shape end Sq;
public class Big : Circle end Big;
public interface IFoo method Id(): int; end IFoo;
public class FA : IFoo public method Id(): int return 1; end Id; end FA;
public class FB public method Id(): int return 2; end Id; end FB;
record Pt(X: int, Y: int);
class Bx
  public static method I(v: int): Object? var o: Object? := v; return o; end I;
  public static method L(v: long): Object? var o: Object? := v; return o; end L;
  public static method D(v: double): Object? var o: Object? := v; return o; end D;
  public static method Z(v: bool): Object? var o: Object? := v; return o; end Z;
  public static method P(v: Pt): Object? var o: Object? := v; return o; end P;
end Bx;
record SP(S: Shape?, B: Circle, O: Object?);
class C
  public static method T(p: SP): string
    return (case p of SP(var s: Shape, var b: Shape, var o: Object) => "allsuper", _ => "none");
  end T;
  public static method U(p: SP): string
    return (case p of SP(var s: Big, var b: Big, var o) => "big", SP(var s, var b: Big, var o) => "bigb", SP(var s, var b: Circle, var o) => "circb", _ => "none");
  end U;
  public static method Main(): int
    Console.WriteLine(T(new SP(new Sq(), new Circle(), 1)));
    Console.WriteLine(T(new SP(null, new Circle(), 1)));
    Console.WriteLine(U(new SP(new Big(), new Big(), 1)));
    Console.WriteLine(U(new SP(new Circle(), new Big(), 1)));
    Console.WriteLine(U(new SP(new Circle(), new Circle(), 1)));
    return 0;
  end Main;
end C;)BZL"),
        std::string("allsuper\nnone\nbig\nbigb\ncircb\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_type_test_7003, mixed_slot_kinds) {
    BEZEL_EXPECT_EQ(
        plainOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
public class Sq : Shape end Sq;
public class Big : Circle end Big;
public interface IFoo method Id(): int; end IFoo;
public class FA : IFoo public method Id(): int return 1; end Id; end FA;
public class FB public method Id(): int return 2; end Id; end FB;
record Pt(X: int, Y: int);
class Bx
  public static method I(v: int): Object? var o: Object? := v; return o; end I;
  public static method L(v: long): Object? var o: Object? := v; return o; end L;
  public static method D(v: double): Object? var o: Object? := v; return o; end D;
  public static method Z(v: bool): Object? var o: Object? := v; return o; end Z;
  public static method P(v: Pt): Object? var o: Object? := v; return o; end P;
end Bx;
record SP(A: Shape?, B: int?, C: Object?);
class C
  public static method T(p: SP): string
    return (case p of SP(var a: Circle, var b: int, var c: int) => "cbi", SP(var a: Circle, var b: int, var c: string) => "cbs", SP(var a: Sq, var b, var c: Circle) => "sqc", SP(var a, var b: int, var c: Circle) => "bc", _ => "other");
  end T;
  public static method Main(): int
    Console.WriteLine(T(new SP(new Circle(), 1, Bx.I(2))));
    Console.WriteLine(T(new SP(new Circle(), 1, "s")));
    Console.WriteLine(T(new SP(new Sq(), null, new Circle())));
    Console.WriteLine(T(new SP(null, 4, new Circle())));
    Console.WriteLine(T(new SP(new Circle(), null, Bx.I(2))));
    Console.WriteLine(T(new SP(new Sq(), 1, Bx.I(2))));
    return 0;
  end Main;
end C;)BZL"),
        std::string("cbi\ncbs\nsqc\nbc\nother\nother\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_type_test_7003, string_and_object_slots) {
    BEZEL_EXPECT_EQ(
        plainOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
public class Sq : Shape end Sq;
public class Big : Circle end Big;
public interface IFoo method Id(): int; end IFoo;
public class FA : IFoo public method Id(): int return 1; end Id; end FA;
public class FB public method Id(): int return 2; end Id; end FB;
record Pt(X: int, Y: int);
class Bx
  public static method I(v: int): Object? var o: Object? := v; return o; end I;
  public static method L(v: long): Object? var o: Object? := v; return o; end L;
  public static method D(v: double): Object? var o: Object? := v; return o; end D;
  public static method Z(v: bool): Object? var o: Object? := v; return o; end Z;
  public static method P(v: Pt): Object? var o: Object? := v; return o; end P;
end Bx;
record SS(S: string?, O: Object?);
class C
  public static method T(r: SS): string
    return (case r of SS(var s: string, var o: string) => "both" + s + o, SS(var s: string, var o) => "s" + s, SS(var s, var o: string) => "o" + o, _ => "none");
  end T;
  public static method Main(): int
    Console.WriteLine(T(new SS("a", "b")));
    Console.WriteLine(T(new SS("a", Bx.I(3))));
    Console.WriteLine(T(new SS(null, "b")));
    Console.WriteLine(T(new SS(null, new Circle())));
    Console.WriteLine(T(new SS("a", new Circle())));
    return 0;
  end Main;
end C;)BZL"),
        std::string("bothab\nsa\nob\nnone\nsa\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_type_test_7003, nested_choice_payloads) {
    BEZEL_EXPECT_EQ(
        plainOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
public class Sq : Shape end Sq;
public class Big : Circle end Big;
public interface IFoo method Id(): int; end IFoo;
public class FA : IFoo public method Id(): int return 1; end Id; end FA;
public class FB public method Id(): int return 2; end Id; end FB;
record Pt(X: int, Y: int);
class Bx
  public static method I(v: int): Object? var o: Object? := v; return o; end I;
  public static method L(v: long): Object? var o: Object? := v; return o; end L;
  public static method D(v: double): Object? var o: Object? := v; return o; end D;
  public static method Z(v: bool): Object? var o: Object? := v; return o; end Z;
  public static method P(v: Pt): Object? var o: Object? := v; return o; end P;
end Bx;
choice Inner A(S: Shape?), B(N: int?); end Inner;
choice Outer W(I: Inner), V(O: Object?); end Outer;
class C
  public static method T(o: Outer): string
    return (case o of W(A(var s: Circle)) => "wac", W(A(var s: Sq)) => "was", W(B(var n: int)) => "wb" + n.ToString(), W(_) => "wother", V(var x: Inner) => "vinner", V(var x: int) => "vint", _ => "none");
  end T;
  public static method Main(): int
    Console.WriteLine(T(Outer.W(Inner.A(new Circle()))));
    Console.WriteLine(T(Outer.W(Inner.A(new Sq()))));
    Console.WriteLine(T(Outer.W(Inner.A(null))));
    Console.WriteLine(T(Outer.W(Inner.B(4))));
    Console.WriteLine(T(Outer.W(Inner.B(null))));
    Console.WriteLine(T(Outer.V(Bx.I(12))));
    Console.WriteLine(T(Outer.V(null)));
    return 0;
  end Main;
end C;)BZL"),
        std::string("wac\nwas\nwother\nwb4\nwother\nvint\nnone\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_type_test_7003, inside_async_body) {
    BEZEL_EXPECT_EQ(
        plainOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
public class Sq : Shape end Sq;
public class Big : Circle end Big;
public interface IFoo method Id(): int; end IFoo;
public class FA : IFoo public method Id(): int return 1; end Id; end FA;
public class FB public method Id(): int return 2; end Id; end FB;
record Pt(X: int, Y: int);
record SP(S: Shape?, I: int?);
class C
  public static async method F(k: int): Task<string>
    await Task.Delay(1);
    var p: SP := (case k of 0 => new SP(new Circle(), 5), 1 => new SP(new Sq(), 6), 2 => new SP(new Sq(), null), _ => new SP(null, null));
    var r := (case p of SP(var c: Circle, var i: int) => "circle" + i.ToString(), SP(var s, var i: int) => "int" + i.ToString(), _ => "other");
    await Task.Delay(1);
    return r;
  end F;
  public static async method Main(): Task<int>
    Console.WriteLine(await F(0));
    Console.WriteLine(await F(1));
    Console.WriteLine(await F(2));
    Console.WriteLine(await F(3));
    return 0;
  end Main;
end C;)BZL"),
        std::string("circle5\nint6\nother\nother\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_type_test_7003, inside_iterator_body) {
    BEZEL_EXPECT_EQ(
        checkedOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
public class Sq : Shape end Sq;
public class Big : Circle end Big;
public interface IFoo method Id(): int; end IFoo;
public class FA : IFoo public method Id(): int return 1; end Id; end FA;
public class FB public method Id(): int return 2; end Id; end FB;
record Pt(X: int, Y: int);
record SP(S: Shape?, I: int?);
class C
  public static method Gen(p: SP): IEnumerable<string>
    yield return (case p of SP(var c: Circle, var i: int) => "circle" + i.ToString(), SP(var s, var i: int) => "int" + i.ToString(), _ => "other");
    yield return "end";
  end Gen;
  public static method Main(): int
    for var s in Gen(new SP(new Circle(), 5)) do Console.WriteLine(s); end for;
    for var s in Gen(new SP(new Sq(), 6)) do Console.WriteLine(s); end for;
    for var s in Gen(new SP(new Sq(), null)) do Console.WriteLine(s); end for;
    return 0;
  end Main;
end C;)BZL"),
        std::string("circle5\nend\nint6\nend\nother\nend\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_type_test_7003, async_choice_statement) {
    BEZEL_EXPECT_EQ(
        checkedOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
public class Sq : Shape end Sq;
public class Big : Circle end Big;
public interface IFoo method Id(): int; end IFoo;
public class FA : IFoo public method Id(): int return 1; end Id; end FA;
public class FB public method Id(): int return 2; end Id; end FB;
record Pt(X: int, Y: int);
choice Ch Hold(S: Shape?, N: int?), Nope; end Ch;
class C
  public static async method F(k: int): Task<int>
    await Task.Delay(1);
    using var s1 := new Circle();
    using var s2 := new Sq();
    var c: Ch := (case k of 0 => Ch.Hold(s1, 1), 1 => Ch.Hold(s2, 2), _ => Ch.Hold(s2, null));
    case c of
      Hold(var s: Circle, var n: int) => do return 10 + n; end,
      Hold(var s, var n: int) => do return 20 + n; end,
      _ => do return 0; end
    end case;
  end F;
  public static async method Main(): Task<int>
    Console.WriteLine((await F(0)).ToString());
    Console.WriteLine((await F(1)).ToString());
    Console.WriteLine((await F(2)).ToString());
    return 0;
  end Main;
end C;)BZL"),
        std::string("11\n22\n0\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_type_test_7003, iterator_case_statement) {
    BEZEL_EXPECT_EQ(
        plainOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
public class Sq : Shape end Sq;
public class Big : Circle end Big;
public interface IFoo method Id(): int; end IFoo;
public class FA : IFoo public method Id(): int return 1; end Id; end FA;
public class FB public method Id(): int return 2; end Id; end FB;
record Pt(X: int, Y: int);
record SP(S: Shape?, I: int?);
class C
  public static method Gen(): IEnumerable<int>
    using var items := new Array<SP>();
    items.Add(new SP(new Circle(), 1));
    items.Add(new SP(new Sq(), 2));
    items.Add(new SP(null, null));
    items.Add(new SP(new Big(), 4));
    for var p in items do
      case p of
        SP(var c: Circle, var i: int) => do yield return 100 + i; end,
        SP(var s: Sq, var i: int) => do yield return 200 + i; end,
        _ => do yield return 0; end
      end case;
    end for;
  end Gen;
  public static method Main(): int
    for var v in Gen() do Console.WriteLine(v.ToString()); end for;
    return 0;
  end Main;
end C;)BZL"),
        std::string("101\n202\n0\n104\n"));
}
