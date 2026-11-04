//! case: s14_2_0802
//! doc: spec
//! section: 14.2
//! title: an interface supplying IComparable<int> does not make the record conform to IComparable<Pt>
//! rule: **T is a record** that lists I (or any interface extending I) in its inheritance clause and declares matching members
//! cites: 14.2, 5.1
//! expect: reject
//! diagnostic: IComparable
//! issue: 8139
//! notes: IPair<Pt, int> extends IComparable<int>; no interface in Pt's chain is IComparable<Pt>. Twin: s14_2_0801 swaps the arguments.
module Conf;

public interface IPair<A, B>: IComparable<B>
end IPair;

public record Pt(x: int): IPair<Pt, int>
  public method CompareTo(other: int): int => x - other;
end Pt;

public class Program
  public static method Main()
    var a: Array<Pt> := [new Pt(3), new Pt(1), new Pt(2)];
    Console.WriteLine(a.Min().x);
  end Main;
end Program;
