//! case: s2_7_1108
//! doc: spec
//! section: 2.7
//! title: a derived class whose type parameter is named Guid reads an inherited generic property
//! rule: inside `class Box<Array>`, a bare `Array` is the type parameter ... A type parameter is
//!   found at the link of the declaration that declares it
//! cites: 14.1, 3.6
//! expect: run
//! output: 11
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
  public method Go(): int
    D[0] := 11;
    return D[0];
  end Go;
end Derived;
public class Program
  public static method Main()
    var x := new Derived<int>();
    Console.WriteLine(x.Go());
  end Main;
end Program;
