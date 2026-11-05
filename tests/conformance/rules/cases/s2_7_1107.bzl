//! case: s2_7_1107
//! doc: spec
//! section: 2.7
//! title: the builtin Guid and a type parameter named Guid appear in different classes of one chain
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
public class B<Guid, Z>: A<Guid, Z>
  public constructor()
    base();
  end constructor;
  public method Put(g: Guid, v: Z)
    D[g] := v;
  end Put;
  public method Look(g: Guid): Z
    return D[g];
  end Look;
end B;
public class C<Y>: B<Guid, Y>
  public constructor()
    base();
  end constructor;
end C;
public class E<Guid>: C<Guid>
  public constructor()
    base();
  end constructor;
  public method N(): int
    return D.Count;
  end N;
end E;
public class Program
  public static method Main()
    var e := new E<long>();
    var g := Guid.NewGuid();
    var h := Guid.NewGuid();
    e.Put(g, 40);
    e.Put(h, 2);
    Console.WriteLine(e.N());
    Console.WriteLine(e.Look(g) + e.Look(h));
  end Main;
end Program;
