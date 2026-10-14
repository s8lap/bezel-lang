//! case: s22_6_0089
//! doc: spec
//! section: 22.6
//! title: explicit type arguments that name an enclosing method's type parameter select the nested overload
//! rule: Explicit type arguments may name a type parameter in scope; the list binds the declared parameter and the overload is then ranked by applicability (22.6).
//! cites: 22.6, 14.5
//! expect: run
//! stdout: 2010
//! issue: 6228
module Conf;

public class C
  public method S<V>(x: Array<V>): int => 10;
  public method S<V>(x: Array<Array<V>>): int => 20;
  public method Outer<W>(b: Array<Array<W>>, a: Array<W>): int
    => this.S<W>(b) * 100 + this.S<W>(a);
end C;

public class Program
  public static method Main()
    using var c := new C();
    using var a := new Array<int>();
    using var b := new Array<Array<int>>();
    Console.WriteLine(c.Outer<int>(b, a));
  end Main;
end Program;
