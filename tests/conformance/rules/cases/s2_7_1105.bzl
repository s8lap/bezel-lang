//! case: s2_7_1105
//! doc: spec
//! section: 2.7
//! title: a derived class's type parameter named Guid flows to an inherited property and to an interface list
//! rule: inside `class Box<Array>`, a bare `Array` is the type parameter ... A type parameter is
//!   found at the link of the declaration that declares it
//! cites: 14.1, 3.6
//! expect: run
//! output: 9
//!   9
//! issue: 6530
module Conf;
public interface IGet<K>
  method Get(k: K): int;
end IGet;
public class Base<K>: IGet<K>
  public property D: Dictionary<K, int>;
  public constructor()
    D := new Dictionary<K, int>();
  end constructor;
  public method Get(k: K): int
    return D[k];
  end Get;
end Base;
public class Derived<Guid>: Base<Guid>, IGet<Guid>
  public constructor()
    base();
  end constructor;
  public method Go(k: Guid): int
    D[k] := 9;
    return D[k];
  end Go;
end Derived;
public class Program
  public static method Main()
    var x := new Derived<int>();
    Console.WriteLine(x.Go(1));
    var i: IGet<int> := x;
    Console.WriteLine(i.Get(1));
  end Main;
end Program;
