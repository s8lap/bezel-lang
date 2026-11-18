//! case: s14_2_6424
//! doc: spec
//! section: 14.2
//! title: DateTime, which lists IComparable<DateTime>, satisfies a where T:
//!   IComparable<DateTime> bound
//! rule: that lists I (or any interface extending I) in its inheritance clause and
//!   declares matching members
//! cites: 14.2
//! expect: run
//! stdout: accepted
module Conf;
public class Program
  public static method Main(): int
    return F(DateTime.FromTicks(0L));
  end Main;
  public static method F<T>(x: T): int where T: IComparable<DateTime>
    Console.WriteLine("accepted");
    return 0;
  end F;
end Program;
