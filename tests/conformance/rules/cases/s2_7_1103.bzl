//! case: s2_7_1103
//! doc: spec
//! section: 2.7
//! title: builtin Guid as a base argument coexists with a derived type parameter named Guid
//! rule: inside `class Box<Array>`, a bare `Array` is the type parameter ... A type parameter is
//!   found at the link of the declaration that declares it
//! cites: 14.1, 3.6
//! expect: run
//! output: 2
//!   42
//! issue: 6530
module Conf;
public class A<K, W>
  public property D: Dictionary<K, W>;
  public constructor()
    D := new Dictionary<K, W>();
  end constructor;
end A;
public class B<Y>: A<Guid, Y>
  public constructor()
    base();
  end constructor;
  public method Put(g: Guid, v: Y)
    D[g] := v;
  end Put;
  public method Look(g: Guid): Y
    return D[g];
  end Look;
end B;
public class C<Guid>: B<Guid>
  public constructor()
    base();
  end constructor;
  public method N(): int
    return D.Count;
  end N;
end C;
public class Program
  public static method Main()
    var c := new C<int>();
    var g := Guid.NewGuid();
    var h := Guid.NewGuid();
    c.Put(g, 40);
    c.Put(h, 2);
    Console.WriteLine(c.N());
    Console.WriteLine(c.Look(g) + c.Look(h));
  end Main;
end Program;
