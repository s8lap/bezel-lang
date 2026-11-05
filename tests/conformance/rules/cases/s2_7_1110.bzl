//! case: s2_7_1110
//! doc: spec
//! section: 2.7
//! title: a generic method whose own parameter is named Guid applied to Derived<Guid> receivers
//! rule: inside `class Box<Array>`, a bare `Array` is the type parameter ... A type parameter is
//!   found at the link of the declaration that declares it
//! cites: 14.1, 3.6
//! expect: run
//! output: 22
//!   22
//! issue: 6530
module Conf;
public class Base<K>
  public owned D: Dictionary<K, int>;
  public constructor()
    D := new Dictionary<K, int>();
  end constructor;
end Base;
public class Derived<Guid>: Base<Guid>
  public constructor()
    base();
  end constructor;
end Derived;
public class U
  public static method Use<Guid>(x: Derived<Guid>, k: Guid): int
    x.D[k] := 21;
    return x.D[k] + x.D.Count;
  end Use;
end U;
public class Program
  public static method Main()
    using var x := new Derived<int>();
    Console.WriteLine(U.Use<int>(x, 3));
    using var y := new Derived<string>();
    Console.WriteLine(U.Use<string>(y, "abc"));
  end Main;
end Program;
