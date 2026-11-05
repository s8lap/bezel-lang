//! case: s14_2_1301
//! doc: spec
//! section: 14.2
//! title: a bound IA<V> over IB<Y> extending IComparable<Y> guarantees IComparable<V> for Math.Max<V>
//! rule: a type-bound constraint naming I or an interface extending I
//! cites: 14.2, 27.6
//! expect: run
//! output: 0
module Conf;
public interface IB<Y>: IComparable<Y>
end IB;
public interface IA<X>: IB<X>
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
