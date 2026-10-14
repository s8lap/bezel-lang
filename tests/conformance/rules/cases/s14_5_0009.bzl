//! case: s14_5_0009
//! doc: spec
//! section: 14.5
//! title: an explicit list longer than a single generic candidate declares is rejected
//! rule: A shorter or a longer list is a compile error; there are no partial lists.
//!   The error is reported only when no candidate declares that many type parameters.
//! cites: 14.5
//! expect: reject
//! diagnostic: type arguments
//! issue: 6228
module Conf;

public class C
  public method S<V>(x: V): int => 10;
end C;

public class Program
  public static method Main()
    using var c := new C();
    Console.WriteLine(c.S<int, int>(1));
  end Main;
end Program;
