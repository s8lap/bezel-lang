//! case: l2_6_4801
//! doc: stdlib
//! section: 2.6
//! title: Array<Guid>.Sort() orders by the Guid CompareTo: Empty, then ...0001, then all-F
//! rule: public record Guid: IComparable<Guid>, IEquatable<Guid>
//! cites: 4.4, 14.2
//! expect: run
//! output: True
//!   True
//!   True
module Conf;

public class Program
  public static method Main()
    var a: Array<Guid> := [Guid.Parse("ffffffff-ffff-ffff-ffff-ffffffffffff"), Guid.Parse("00000000-0000-0000-0000-000000000001"), Guid.Empty];
    a.Sort();
    Console.WriteLine(a[0] = Guid.Empty);
    Console.WriteLine(a[1] = Guid.Parse("00000000-0000-0000-0000-000000000001"));
    Console.WriteLine(a[2] = Guid.Parse("ffffffff-ffff-ffff-ffff-ffffffffffff"));
  end Main;
end Program;
