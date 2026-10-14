//! case: s14_5_0010
//! doc: spec
//! section: 14.5
//! title: an explicit type-argument list that matches no overload's argument types is rejected
//! rule: An explicit type-argument list binds the declared type parameters in declaration order; when a full explicit list is supplied, no inference runs.
//!   The bound parameter types then take part in overload applicability (22.6): when none of the candidates applies, the call is an error.
//! cites: 14.5, 22.6
//! expect: reject
//! diagnostic: applicability
//! issue: 6228
module Conf;

public class C
  public method S<V>(x: Array<V>): int => 10;
  public method S<V>(x: Array<Array<V>>): int => 20;
end C;

public class Program
  public static method Main()
    using var c := new C();
    using var b := new Array<Array<int>>();
    Console.WriteLine(c.S<string>(b));
  end Main;
end Program;
