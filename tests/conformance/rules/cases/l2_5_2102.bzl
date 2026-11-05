//! case: l2_5_2102
//! doc: stdlib
//! section: 2.5
//! title: a compound DateTime += / -= landing exactly on MaxValue or on
//!   tick 0 is in range
//! rule: The `Add*` family, `Add`, `Subtract(span: TimeSpan)`, and
//!   `DateTime.FromTicks` raise `OverflowException` when the result falls
//!   outside `MinValue..MaxValue`
//! cites: 6.6
//! expect: run
//! output: True
//!   0
//!   3155378975999999998
//! notes: Green on base and on the staging tip: the in-range boundary
//!   controls of the #8245 range check, which must not over-reject.
module Conf;

public class Program
  public static method Main()
    var b := new Array<DateTime>(1, DateTime.FromTicks(DateTime.MaxValue.Ticks - 5L));
    b[0] += TimeSpan.FromTicks(5L);
    Console.WriteLine(b[0].Ticks = DateTime.MaxValue.Ticks);
    var c := new Array<DateTime>(1, DateTime.FromTicks(5L));
    c[0] -= TimeSpan.FromTicks(5L);
    Console.WriteLine(c[0].Ticks);
    var d := new Array<DateTime>(1, DateTime.MaxValue);
    d[0] += TimeSpan.FromTicks(-1L);
    Console.WriteLine(d[0].Ticks);
  end Main;
end Program;
