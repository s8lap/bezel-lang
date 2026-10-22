//! case: l2_18_0011
//! doc: stdlib
//! section: 2.18
//! title: the TimeSpan constant form renders the documented examples
//! rule: "`TimeSpan.FromMinutes(90.5)` renders `01:30:30`" and "`TimeSpan.FromDays(1.5)` renders `1.12:00:00`" ([stdlib §2.18]); "The no-format `ToString()`, `ToString("c")` and an interpolation hole with or without `:c` produce this one rendering" ([stdlib §2.18]). A one-tick span renders its sub-second remainder as seven digits, and a minus sign is shown for a negative span.
//! expect: run
//! output: 01:30:30
//!   00:00:00.0000001
//!   -00:00:00.0000001
//!   -00:00:00.0000001
//!   1.12:00:00
//!   00:00:03
module Conf;
public class Program
  public static method Main(): int
    var a := TimeSpan.FromMinutes(90.5);
    Console.WriteLine(a.ToString());
    var one := TimeSpan.FromTicks(1);
    var m1 := TimeSpan.FromTicks(-1);
    Console.WriteLine(one.ToString("c"));
    Console.WriteLine(m1.ToString());
    Console.WriteLine($"{m1:c}");
    Console.WriteLine(TimeSpan.FromDays(1.5).ToString());
    Console.WriteLine(TimeSpan.FromSeconds(3.0).ToString());
    return 0;
  end Main;
end Program;
