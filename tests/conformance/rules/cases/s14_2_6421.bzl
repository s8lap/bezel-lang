//! case: s14_2_6421
//! doc: spec
//! section: 14.2
//! title: a record listing IComparable<DateTime> satisfies a where T:
//!   IComparable<DateTime> bound
//! rule: that lists I (or any interface extending I) in its inheritance clause and
//!   declares matching members
//! cites: 14.2
//! expect: run
//! stdout: accepted
module Conf;
public class Program
  public static method Main(): int
    return F(new RecD());
  end Main;
  public static method F<T>(x: T): int where T: IComparable<DateTime>
    Console.WriteLine("accepted");
    return 0;
  end F;
end Program;
public record RecD: IComparable<DateTime>
  public method CompareTo(other: DateTime): int
    return 0;
  end CompareTo;
end RecD;
