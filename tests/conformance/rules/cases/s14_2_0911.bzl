//! case: s14_2_0911
//! doc: spec
//! section: 14.2
//! title: Math.Max<V> where V: IComparable<W> names a different instantiation and is rejected
//! rule: the constraint is decided at that site, by `P`'s own constraints. `P` satisfies the
//!   constraint exactly when its constraints guarantee it at every substitution they admit,
//!   as a matter of language rule ... a type-bound constraint naming I or an interface
//!   extending I, or a class bound naming a class that conforms to I under rule 1, directly
//!   or through a `where T: U` chain.
//! cites: 14.5, 27.6
//! expect: reject
//! diagnostic: constraint
//! issue: 6295
module Conf;

public class Helper
  public static method F<V, W>(): (V, V) -> V where V: IComparable<W>
    return Math.Max<V>;
  end F;
end Helper;

public class Program
  public static method Main()
    Console.WriteLine(0);
  end Main;
end Program;
