//! case: s14_2_0801
//! doc: spec
//! section: 14.2
//! title: a record conforms to IComparable<Pt> through an extending interface, so no-selector Min is accepted
//! rule: **T is a record** that lists I (or any interface extending I) in its inheritance clause and declares matching members
//! cites: 14.2, 5.1
//! expect: run
//! output: 1
//!   3
//!   1
//!   3
//! issue: 8139
//! notes: IPair<int, Pt> extends IComparable<Pt>, so Pt conforms to IComparable<Pt> (rule 2: any interface extending I). The Where(...).Min() shape is not asserted: it is tracked by open #8161.
module Conf;

public interface IPair<A, B>: IComparable<B>
end IPair;

public record Pt(x: int): IPair<int, Pt>
  public method CompareTo(other: Pt): int => (if x > other.x then 1 else (if x < other.x then -1 else 0));
end Pt;

public class Program
  public static method Main()
    var a: Array<Pt> := [new Pt(3), new Pt(1), new Pt(2)];
    Console.WriteLine(a.Min().x);
    Console.WriteLine(a.Max().x);
    Console.WriteLine(a.MinOrNull()!.x);
    Console.WriteLine(a.MaxOrNull()!.x);
  end Main;
end Program;
