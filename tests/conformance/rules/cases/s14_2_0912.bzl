//! case: s14_2_0912
//! doc: spec
//! section: 14.2
//! title: Math.Max<V> inside a method whose enclosing class constrains a different parameter W is rejected
//! rule: the constraint is decided at that site, by `P`'s own constraints. `P` satisfies the
//!   constraint exactly when its constraints guarantee it at every substitution they admit,
//!   as a matter of language rule
//! cites: 14.5, 27.6
//! expect: reject
//! diagnostic: constraint
//! issue: 6295
module Conf;

public class Box<W> where W: IComparable<W>
  public static method F<V>(): (V, V) -> V
    return Math.Max<V>;
  end F;
end Box;

public class Program
  public static method Main()
    Console.WriteLine(0);
  end Main;
end Program;
