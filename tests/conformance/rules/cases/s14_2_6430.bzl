//! case: s14_2_6430
//! doc: spec
//! section: 14.2
//! title: a bound spelled through a type alias of DateTime admits DateTime itself
//! rule: that lists I (or any interface extending I) in its inheritance clause and
//!   declares matching members
//! cites: 14.2
//! expect: run
//! stdout: accepted
module Conf;
type DT := DateTime;
public class Program
  public static method Main(): int
    return F(DateTime.FromTicks(0L));
  end Main;
  public static method F<T>(x: T): int where T: IComparable<DT>
    Console.WriteLine("accepted");
    return 0;
  end F;
end Program;
