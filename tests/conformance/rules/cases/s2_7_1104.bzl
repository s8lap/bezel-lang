//! case: s2_7_1104
//! doc: spec
//! section: 2.7
//! title: type parameters named Guid and Range on a class and its generic method resolve to themselves
//! rule: inside `class Box<Array>`, a bare `Array` is the type parameter ... A type parameter is
//!   found at the link of the declaration that declares it
//! cites: 14.1, 3.6
//! expect: run
//! output: 12
//!   12
//! issue: 6530
module Conf;
public class Base<K>
  public property D: Dictionary<K, int>;
  public constructor()
    D := new Dictionary<K, int>();
  end constructor;
end Base;
public class Derived<Guid>: Base<Guid>
  public constructor()
    base();
  end constructor;
  public method M<Range>(k: Guid, u: Range): int
    D[k] := 11;
    return D[k] + D.Count;
  end M;
end Derived;
public class Program
  public static method Main()
    var x := new Derived<int>();
    Console.WriteLine(x.M<string>(0, "a"));
    var y := new Derived<string>();
    Console.WriteLine(y.M<long>("k", 3));
  end Main;
end Program;
