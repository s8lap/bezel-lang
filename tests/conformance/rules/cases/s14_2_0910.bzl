//! case: s14_2_0910
//! doc: spec
//! section: 14.2
//! title: Math.Max<V> where V is constrained only through a different type parameter W is rejected
//! rule: the constraint is decided at that site, by `P`'s own constraints. `P` satisfies the
//!   constraint exactly when its constraints guarantee it at every substitution they admit,
//!   as a matter of language rule
//! cites: 14.5, 27.6
//! expect: reject
//! diagnostic: constraint
//! issue: 6295
module Conf;

public class Helper
  public static method F<V, W>(): (V, V) -> V where W: IComparable<W>
    return Math.Max<V>;
  end F;
end Helper;

public class Program
  public static method Main()
    Console.WriteLine(0);
  end Main;
end Program;
