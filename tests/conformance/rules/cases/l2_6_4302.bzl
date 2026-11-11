//! case: l2_6_4302
//! doc: stdlib
//! section: 2.6
//! title: a Guid bound to an IComparable<Guid> local compares against itself and a higher value
//! rule: public record Guid: IComparable<Guid>, IEquatable<Guid>
//! cites: 2.6, 2.8, 5.1
//! expect: run
//! output: True
//!   True
//! issue: 8288
module Conf;

public class Program
  public static method Main()
    var hi := Guid.Parse("22222222-2222-2222-2222-222222222222");
    var q := Guid.Empty;
    var g: IComparable<Guid> := q;
    Console.WriteLine(g.CompareTo(hi) < 0);
    Console.WriteLine(g.CompareTo(Guid.Empty) = 0);
  end Main;
end Program;
