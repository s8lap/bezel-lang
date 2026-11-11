//! case: l2_6_4301
//! doc: stdlib
//! section: 2.6
//! title: a Guid passed at an IComparable<Guid> parameter orders lower, higher and equal values
//! rule: public record Guid: IComparable<Guid>, IEquatable<Guid>
//! cites: 2.6, 2.8, 5.1
//! expect: run
//! output: True
//!   True
//!   True
//! issue: 8288
module Conf;

public class Program
  static method Cmp(x: IComparable<Guid>, y: Guid): int
    return x.CompareTo(y);
  end Cmp;

  public static method Main()
    var lo := Guid.Parse("00000001-0000-0000-0000-000000000000");
    var hi := Guid.Parse("22222222-2222-2222-2222-222222222222");
    Console.WriteLine(Cmp(lo, hi) < 0);
    Console.WriteLine(Cmp(hi, lo) > 0);
    Console.WriteLine(Cmp(lo, lo) = 0);
  end Main;
end Program;
