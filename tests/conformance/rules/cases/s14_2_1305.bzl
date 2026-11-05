//! case: s14_2_1305
//! doc: spec
//! section: 14.2
//! title: a bound whose base fixes IComparable<int> does not guarantee IComparable<V> for Math.Max<V>
//! rule: `P` satisfies the constraint exactly when its constraints guarantee it at every substitution
//!   they admit
//! cites: 14.2, 27.6
//! expect: reject
//! diagnostic: constraint
module Conf;
public interface IB<Y>: IComparable<Y>
end IB;
public interface IA<X>: IB<int>
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
