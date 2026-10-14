//! case: s14_5_0012
//! doc: spec
//! section: 14.5
//! title: an explicit list reaches only generic candidates while a plain call prefers the non-generic overload
//! rule: Where the name is overloaded, the list's length selects first: a non-generic candidate takes no list.
//!   Without a list, the non-generic candidate is preferred (22.6).
//! cites: 14.5, 22.6
//! expect: run
//! stdout: 10 5 10
//! issue: 6228
module Conf;

public class C
  public method S(x: int): int => 5;
  public method S<V>(x: V): int => 10;
end C;

public class Program
  public static method Main()
    using var c := new C();
    Console.WriteLine($"{c.S<int>(1)} {c.S(1)} {c.S<string>("a")}");
  end Main;
end Program;
