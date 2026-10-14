//! case: s12_1_0050
//! doc: spec
//! section: 12.1
//! title: a generic-method override in a class two generic levels down runs through a base-typed receiver
//! rule: To override a virtual member in a derived class, mark the matching
//!   member `override`. Visibility, return type, parameter types, and
//!   parameter modifiers must match the base member exactly (v0.338, §12.1),
//!   and a base clause instantiates a generic base at its own arguments
//!   (§14.1), so a class that does not override a member runs the nearest
//!   override above it.
//! cites: 12.1, 14.1
//! expect: run
//! issue: 6298
//! output: 1
//!   3
//!   3
module Conf;

public class A<T>
  public constructor()
  end constructor;

  public virtual method Q<U>(e: U, t: T): int => 1;
end A;

public class M<T>: A<T>
  public constructor()
  end constructor;
end M;

public class D: M<int>
  public constructor()
  end constructor;

  public override method Q<V>(e: V, t: int): int => 3;
end D;

public class E: D
  public constructor()
  end constructor;
end E;

public class Program
  public static method Main(): int
    var a: A<int> := new M<int>();
    var d: A<int> := new D();
    var e: A<int> := new E();
    Console.WriteLine(a.Q<string>("x", 1));
    Console.WriteLine(d.Q<string>("x", 1));
    Console.WriteLine(e.Q<string>("x", 1));
    return 0;
  end Main;
end Program;
