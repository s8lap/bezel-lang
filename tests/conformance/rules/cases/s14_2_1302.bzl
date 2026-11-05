//! case: s14_2_1302
//! doc: spec
//! section: 14.2
//! title: a base IB<Y, Z> extending IComparable<Z> guarantees IComparable<V> when V fills slot Z
//! rule: a type-bound constraint naming I or an interface extending I
//! cites: 14.2, 27.6
//! expect: run
//! output: 0
module Conf;
public interface IB<Y, Z>: IComparable<Z>
end IB;
public interface IA<X>: IB<int, X>
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
