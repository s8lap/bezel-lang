//! case: s14_5_0011
//! doc: spec
//! section: 14.5
//! title: an explicit type argument that the argument does not convert to is rejected
//! rule: An explicit type-argument list binds the declared type parameters in declaration order and no inference runs; the argument must then convert to the parameter type the list binds.
//! cites: 14.5, 22.6
//! expect: reject
//! diagnostic: convert
//! issue: 6228
module Conf;

public class C
  public method S<V>(x: V): int => 10;
end C;

public class Program
  public static method Main()
    using var c := new C();
    Console.WriteLine(c.S<int>("x"));
  end Main;
end Program;
