//! case: s14_5_0013
//! doc: spec
//! section: 14.5
//! title: explicit constructed type arguments select between a value overload and an array overload
//! rule: An explicit type-argument list binds the declared type parameters in declaration order and no inference runs; the bound parameter types then rank the applicable overloads (22.6).
//! cites: 14.5, 22.6
//! expect: run
//! output: 20
//!   10
//!   20
//! issue: 6228
module Conf;

public class C
  public method S<V>(x: V): int => 10;
  public method S<V>(x: Array<V>): int => 20;
end C;

public class Program
  public static method Main()
    using var c := new C();
    using var a := new Array<int>();
    using var b := new Array<Array<int>>();
    Console.WriteLine(c.S<Array<int>>(b));
    Console.WriteLine(c.S<Array<int>>(a));
    Console.WriteLine(c.S<int>(a));
  end Main;
end Program;
