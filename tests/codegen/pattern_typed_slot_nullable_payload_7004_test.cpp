// Spec-derived regression test for issue #7004.
//
// Spec §19.5 (v0.340): "A typed binding pattern `var n: T` also matches only non-null values", so `var i: int` over an `int?` slot is a presence test and binds an `int`; §19.3 (v0.340) types the binding by its annotation.
//
// A typed binding `var i: int` over an `int?` / `long?` / `double?` slot matches only a present value and binds the UNWRAPPED payload (the top-level #738 behaviour); over an `Object` slot holding an int it unboxes (the top-level #746 behaviour).
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

BEZEL_TEST(codegen_pattern_typed_slot_nullable_payload_7004, nullable_primitive_slots) {
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
record NP(I: int?, L: long?, D: double?, B: bool?);
class C
  public static method T(r: NP): string
    return (case r of NP(var i: int, var l: long, var d: double, var b: bool) => "all:" + i.ToString() + "," + l.ToString() + "," + d.ToString() + "," + b.ToString(), NP(var i: int, var l, var d, var b) => "i:" + i.ToString(), NP(var i, var l: long, var d, var b) => "l:" + l.ToString(), NP(var i, var l, var d: double, var b) => "d:" + d.ToString(), _ => "none");
  end T;
  public static method Main(): int
    Console.WriteLine(T(new NP(1, 2L, 3.5, true)));
    Console.WriteLine(T(new NP(1, null, null, null)));
    Console.WriteLine(T(new NP(null, 123456789012L, null, null)));
    Console.WriteLine(T(new NP(null, null, 2.25, null)));
    Console.WriteLine(T(new NP(null, null, null, true)));
    Console.WriteLine(T(new NP(null, null, null, null)));
    Console.WriteLine(T(new NP(-5, null, null, null)));
    return 0;
  end Main;
end C;)BZL"),
        std::string("all:1,2,3.5,True\ni:1\nl:123456789012\nd:2.25\nnone\nnone\ni:-5\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_nullable_payload_7004, nullable_record_slot) {
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
record Ptn(P: Pt?, Q: int);
class C
  public static method T(r: Ptn): string
    return (case r of Ptn(var p: Pt, var q) => "pt" + p.X.ToString() + "," + p.Y.ToString(), _ => "none");
  end T;
  public static method Main(): int
    Console.WriteLine(T(new Ptn(new Pt(3, 4), 0)));
    Console.WriteLine(T(new Ptn(null, 0)));
    return 0;
  end Main;
end C;)BZL"),
        std::string("pt3,4\nnone\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_nullable_payload_7004, guards_over_nullable_slot) {
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
    return (case p of SP(var c: Circle, var i: int) when i > 3 => "bigcircle", SP(var c: Circle, var i: int) => "circle" + i.ToString(), SP(var s, var i: int) when i = 0 => "zero", SP(var s, var i) => "any", _ => "other");
  end T;
  public static method Main(): int
    Console.WriteLine(T(new SP(new Circle(), 9)));
    Console.WriteLine(T(new SP(new Circle(), 2)));
    Console.WriteLine(T(new SP(new Sq(), 0)));
    Console.WriteLine(T(new SP(new Sq(), 5)));
    Console.WriteLine(T(new SP(new Sq(), null)));
    Console.WriteLine(T(new SP(new Circle(), null)));
    return 0;
  end Main;
end C;)BZL"),
        std::string("bigcircle\ncircle2\nzero\nany\nany\nany\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_nullable_payload_7004, object_slot_unboxes_value_type) {
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
record OB(O: Object?, K: int);
choice Ch Box(O: Object?), Nope; end Ch;
class C
  public static method T(r: OB): string
    return (case r of OB(var n: int, var k) => "int" + (n + 1).ToString(), OB(var s: string, var k) => "str" + s, OB(var b: bool, var k) => "bool", OB(var l: long, var k) => "long", OB(var p: Pt, var k) => "pt" + p.Y.ToString(), OB(var x: Circle, var k) => "circ", _ => "other");
  end T;
  public static method U(c: Ch): string
    return (case c of Box(var n: int) => "int" + (n + 1).ToString(), Box(var s: string) => "str" + s, Box(var p: Pt) => "pt" + p.X.ToString(), _ => "other");
  end U;
  public static method Main(): int
    Console.WriteLine(T(new OB(Bx.I(41), 0)));
    Console.WriteLine(T(new OB("hey", 0)));
    Console.WriteLine(T(new OB(Bx.Z(true), 0)));
    Console.WriteLine(T(new OB(Bx.L(5L), 0)));
    Console.WriteLine(T(new OB(Bx.P(new Pt(8, 9)), 0)));
    Console.WriteLine(T(new OB(new Circle(), 0)));
    Console.WriteLine(T(new OB(new Sq(), 0)));
    Console.WriteLine(T(new OB(null, 0)));
    Console.WriteLine(U(Ch.Box(Bx.I(41))));
    Console.WriteLine(U(Ch.Box("hey")));
    Console.WriteLine(U(Ch.Box(Bx.P(new Pt(8, 9)))));
    Console.WriteLine(U(Ch.Box(Bx.D(2.5))));
    Console.WriteLine(U(Ch.Box(null)));
    return 0;
  end Main;
end C;)BZL"),
        std::string("int42\nstrhey\nbool\nlong\npt9\ncirc\nother\nother\nint42\nstrhey\npt8\nother\nother\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_nullable_payload_7004, object_slot_named_statement) {
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
record OB(O: Object?, K: int);
class C
  public static method Main(): int
    var o: OB := new OB(Bx.I(7), 1);
    case o of
      OB(O = var n: int, K = var k) => Console.WriteLine("int" + (n * 6).ToString()),
      _ => Console.WriteLine("other")
    end case;
    var q: OB := new OB("s", 1);
    case q of
      OB(O = var n: int, K = var k) => Console.WriteLine("int"),
      OB(O = var s: string, K = var k) => Console.WriteLine("str" + s),
      _ => Console.WriteLine("other")
    end case;
    return 0;
  end Main;
end C;)BZL"),
        std::string("int42\nstrs\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_nullable_payload_7004, generic_record_and_choice_slots) {
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
record Box<T>(V: T, W: int);
choice Gc<T> Has(V: T), Non; end Gc;
class C
  public static method T(b: Box<int?>): string
    return (case b of Box(var v: int, var w) => "int" + v.ToString(), _ => "none");
  end T;
  public static method U(b: Box<Shape?>): string
    return (case b of Box(var v: Circle, var w) => "circle", Box(var v: Shape, var w) => "shape", _ => "none");
  end U;
  public static method G(c: Gc<Object?>): string
    return (case c of Has(var v: int) => "int" + v.ToString(), Has(var v: Circle) => "circle", Has(var v: Object) => "obj", _ => "none");
  end G;
  public static method Main(): int
    Console.WriteLine(T(new Box<int?>(5, 1)));
    Console.WriteLine(T(new Box<int?>(null, 1)));
    Console.WriteLine(U(new Box<Shape?>(new Circle(), 1)));
    Console.WriteLine(U(new Box<Shape?>(new Sq(), 1)));
    Console.WriteLine(U(new Box<Shape?>(null, 1)));
    Console.WriteLine(G(Gc<Object?>.Has(Bx.I(3))));
    Console.WriteLine(G(Gc<Object?>.Has(new Circle())));
    Console.WriteLine(G(Gc<Object?>.Has(new Sq())));
    Console.WriteLine(G(Gc<Object?>.Has(null)));
    return 0;
  end Main;
end C;)BZL"),
        std::string("int5\nnone\ncircle\nshape\nnone\nint3\ncircle\nobj\nnone\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_nullable_payload_7004, binding_reads_payload_in_loop) {
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
  public static method T(p: SP): int
    return (case p of SP(var s, var i: int) => i * 2 + 1, _ => -1);
  end T;
  public static method Main(): int
    var k: int := 0; var tot: int := 0;
    while k < 50 do
      tot := tot + T(new SP(null, k)) + T(new SP(null, null));
      k := k + 1;
    end while;
    Console.WriteLine(tot.ToString());
    Console.WriteLine(T(new SP(null, 1000000)).ToString());
    Console.WriteLine(T(new SP(null, -1000000)).ToString());
    return 0;
  end Main;
end C;)BZL"),
        std::string("2450\n2000001\n-1999999\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_nullable_payload_7004, enum_and_string_slots) {
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
enum Color Red, Green; end Color;
record ER(C: Object?, E: Color?, S: string?);
class C
  public static method T(r: ER): string
    return (case r of ER(var c: Color, var e, var s) => "boxedcolor", ER(var c, var e: Color, var s) => "color", ER(var c, var e, var s: string) => "str" + s, _ => "none");
  end T;
  public static method Main(): int
    Console.WriteLine(T(new ER(null, null, null)));
    Console.WriteLine(T(new ER(null, Color.Green, null)));
    Console.WriteLine(T(new ER(null, null, "z")));
    Console.WriteLine(T(new ER(Bx.I(5), null, null)));
    Console.WriteLine(T(new ER(null, null, null)));
    return 0;
  end Main;
end C;)BZL"),
        std::string("none\ncolor\nstrz\nnone\nnone\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_nullable_payload_7004, long_and_double_slots) {
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
record LD(L: long?, D: double?);
class C
  public static method T(r: LD): string
    return (case r of LD(var l: long, var d: double) => "both" + (l + 1L).ToString() + "/" + (d * 2.0).ToString(), LD(var l: long, var d) => "l" + l.ToString(), LD(var l, var d: double) => "d" + d.ToString(), _ => "none");
  end T;
  public static method Main(): int
    Console.WriteLine(T(new LD(9000000000L, 1.5)));
    Console.WriteLine(T(new LD(9000000000L, null)));
    Console.WriteLine(T(new LD(null, 0.125)));
    Console.WriteLine(T(new LD(null, null)));
    return 0;
  end Main;
end C;)BZL"),
        std::string("both9000000001/3\nl9000000000\nd0.125\nnone\n"));
}
