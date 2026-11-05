//! case: s14_2_1308
//! doc: spec
//! section: 14.2
//! title: V bounded by W where W: IA<W> is guaranteed IComparable<W>, not IComparable<V>
//! rule: directly or through a `where T: U` chain
//! cites: 14.2, 27.6
//! expect: reject
//! diagnostic: constraint
module Conf;
public interface IB<Y>: IComparable<Y>
end IB;
public interface IA<X>: IB<X>
end IA;
public class Helper
  public static method F<V, W>(): (V, V) -> V where V: W where W: IA<W>
    return Math.Max<V>;
  end F;
end Helper;
public class Program
  public static method Main()
    Console.WriteLine(0);
  end Main;
end Program;
