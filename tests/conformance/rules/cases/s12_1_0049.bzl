//! case: s12_1_0049
//! doc: spec
//! section: 12.1
//! title: a generic-method override reached through a base-typed receiver runs the override
//! rule: A method, property, or indexer is non-virtual unless marked
//!   `virtual`. To override a virtual member in a derived class, mark the
//!   matching member `override`. Visibility, return type, parameter types,
//!   and parameter modifiers must match the base member exactly (v0.338,
//!   §12.1); a virtual member called through a base-typed reference is
//!   dispatched to the override.
//! cites: 12.1, 14.1
//! expect: run
//! issue: 6298
//! output: 2
//!   2
//!   2
module Conf;

public class B<T>
  public constructor()
  end constructor;

  public virtual method Q<U>(e: U, t: T): int => 1;
end B;

public class C: B<int>
  public constructor()
  end constructor;

  public override method Q<V>(e: V, t: int): int => 2;
end C;

public class Program
  public static method Main(): int
    var c: C := new C();
    var b: B<int> := c;
    Console.WriteLine(c.Q<string>("x", 1));
    Console.WriteLine(b.Q<string>("x", 1));
    Console.WriteLine(b.Q<int>(5, 1));
    return 0;
  end Main;
end Program;
