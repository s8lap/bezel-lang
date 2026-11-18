//! case: s14_2_6427
//! doc: spec
//! section: 14.2
//! title: a record conforming only to IComparable<Rec> does not satisfy a where T:
//!   IComparable<DateTime> bound
//! rule: that lists I (or any interface extending I) in its inheritance clause and
//!   declares matching members
//! cites: 14.2
//! expect: reject
//! diagnostic: does not satisfy the constraint
module Conf;
public class Program
  public static method Main(): int
    return F(new Rec());
  end Main;
  public static method F<T>(x: T): int where T: IComparable<DateTime>
    Console.WriteLine("accepted");
    return 0;
  end F;
end Program;
public record Rec: IComparable<Rec>
  public method CompareTo(other: Rec): int
    return 0;
  end CompareTo;
end Rec;
