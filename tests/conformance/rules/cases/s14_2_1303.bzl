//! case: s14_2_1303
//! doc: spec
//! section: 14.2
//! title: a three-level interface chain ending at IComparable<Q> guarantees IComparable<V> for Math.Max<V>
//! rule: a type-bound constraint naming I or an interface extending I
//! cites: 14.2, 27.6
//! expect: run
//! output: 0
module Conf;
public interface IC<Q>: IComparable<Q>
end IC;
public interface IB<Y>: IC<Y>
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
