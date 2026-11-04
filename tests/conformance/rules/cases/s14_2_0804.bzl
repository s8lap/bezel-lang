//! case: s14_2_0804
//! doc: spec
//! section: 14.2
//! title: conforming to IComparable<B> for an unrelated record B does not make Pt conform to IComparable<Pt>
//! rule: **T is a record** that lists I (or any interface extending I) in its inheritance clause and declares matching members
//! cites: 14.2, 5.1
//! expect: reject
//! diagnostic: IComparable
//! issue: 8139
//! notes: IOrd<B> extends IComparable<B>, not IComparable<Pt>.
module Conf;

public record B(y: int);

public interface IOrd<T>: IComparable<T>
end IOrd;

public record Pt(x: int): IOrd<B>
  public method CompareTo(other: B): int => x - other.y;
end Pt;

public class Program
  public static method Main()
    var a: Array<Pt> := [new Pt(3), new Pt(1), new Pt(2)];
    Console.WriteLine(a.Min().x);
  end Main;
end Program;
