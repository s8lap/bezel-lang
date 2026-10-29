//! case: l2_5_0010
//! doc: stdlib
//! section: 2.5
//! title: TimeSpan.MinValue and MaxValue are FromTicks of the long extremes and render in the constant form
//! rule: `MinValue` is `FromTicks(long.MinValue)` and `MaxValue` is
//!   `FromTicks(long.MaxValue)`; `TimeSpan.MinValue` renders
//!   `-10675199.02:48:05.4775808` (2.18, the `TimeSpan` constant form). The
//!   no-format `ToString()`, `ToString("c")` produce this one rendering.
//! cites: 2.5, 2.18
//! expect: run
//! output: 10675199.02:48:05.4775807
//!   -9223372036854775808
//!   True
//!   True
//!   -10675199.02:48:05.4775808
//!   -10675199.02:48:05.4775808
//!   10675199.02:48:05.4775807
//!   00:00:00
module Conf;

public class Program
  public static method Main()
    var f: TimeSpan := TimeSpan.MinValue;
    var h := TimeSpan.MaxValue;
    Console.WriteLine(h.ToString("c"));
    Console.WriteLine(f.Ticks);
    Console.WriteLine(h.CompareTo(TimeSpan.Zero) > 0);
    Console.WriteLine(f = TimeSpan.FromTicks(long.MinValue));
    Console.WriteLine(f.ToString("c"));
    Console.WriteLine(TimeSpan.MinValue.ToString());
    Console.WriteLine(TimeSpan.MaxValue.ToString());
    Console.WriteLine(TimeSpan.Zero.ToString());
  end Main;
end Program;
