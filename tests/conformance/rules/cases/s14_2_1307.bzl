//! case: s14_2_1307
//! doc: spec
//! section: 14.2
//! title: a base IB<Y, Z> whose IComparable<Z> slot is filled with int does not guarantee IComparable<V>
//! rule: `P` satisfies the constraint exactly when its constraints guarantee it at every substitution
//!   they admit
//! cites: 14.2, 27.6
//! expect: reject
//! diagnostic: constraint
module Conf;
public interface IB<Y, Z>: IComparable<Z>
end IB;
public interface IA<X>: IB<X, int>
end IA;
public class Helper
  public static method F<V>(): (V, V) -> V where V: IA<V>
    return Math.Max<V>;
  end F;
end Helper;
public class Program
  public static method Main()
    Console.WriteLine(0);
  end Main;
end Program;
