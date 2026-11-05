//! case: s14_2_0908
//! doc: spec
//! section: 14.2
//! title: Math.Clamp<V> inside a method of a class whose own V: IComparable<V> covers it
//! rule: the constraint is decided at that site, by `P`'s own constraints. `P` satisfies the
//!   constraint exactly when its constraints guarantee it at every substitution they admit,
//!   as a matter of language rule
//! cites: 14.5, 27.6
//! expect: run
//! output: 10
//!   1
//! issue: 6295
module Conf;

public class Box<V> where V: IComparable<V>
  public static method C(): (V, V, V) -> V
    return Math.Clamp<V>;
  end C;
end Box;

public class Program
  public static method Main()
    var c: (long, long, long) -> long := Box<long>.C();
    Console.WriteLine(c(99L, 1L, 10L));
    Console.WriteLine(c(-99L, 1L, 10L));
  end Main;
end Program;
