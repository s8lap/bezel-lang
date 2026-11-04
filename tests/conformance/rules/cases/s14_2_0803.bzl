//! case: s14_2_0803
//! doc: spec
//! section: 14.2
//! title: a two-level interface chain reaching only IComparable<int> does not give Pt IComparable<Pt>
//! rule: **T is a record** that lists I (or any interface extending I) in its inheritance clause and declares matching members
//! cites: 14.2, 5.1
//! expect: reject
//! diagnostic: IComparable
//! issue: 8139
//! notes: I2<Pt> extends I1<int>, which extends IComparable<int>; the chain never reaches IComparable<Pt>.
module Conf;

public interface I1<T>: IComparable<T>
end I1;

public interface I2<U>: I1<int>
end I2;

public record Pt(x: int): I2<Pt>
  public method CompareTo(other: int): int => x - other;
end Pt;

public class Program
  public static method Main()
    var a: Array<Pt> := [new Pt(3), new Pt(1), new Pt(2)];
    Console.WriteLine(a.Min().x);
  end Main;
end Program;
