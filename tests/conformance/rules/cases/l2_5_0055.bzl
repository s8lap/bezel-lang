//! case: l2_5_0055
//! doc: stdlib
//! section: 2.5
//! title: millisecond 1000 raises ArgumentOutOfRangeException; millisecond 999 does not
//! rule: The constructors of `DateTime`, `DateOnly` and `TimeOnly` raise
//!   `ArgumentOutOfRangeException` when a component falls outside its range:
//!   `year` 1..9999, `month` 1..12, `day` 1 to the length of that month in
//!   that year of the proleptic Gregorian calendar that ISO 8601 uses
//!   (February 29 exists only in a leap year), `hour` 0..23, `minute` 0..59,
//!   `second` 0..59 and `millisecond` 0..999. `DateOnly` takes the date
//!   ranges and `TimeOnly` the time ranges.
//! cites: 2.5
//! expect: run
//! output: ms999 ok
//!   ms1000 threw
//! issue: 7862
module Conf;

public class Probe
  public static method Ok(): string
    try
      var d := new DateTime(2024, 1, 1, 0, 0, 0, 999);
    catch ArgumentOutOfRangeException => return "threw"; end;
    return "ok";
  end Ok;

  public static method Bad(): string
    try
      var d := new DateTime(2024, 1, 1, 0, 0, 0, 1000);
    catch ArgumentOutOfRangeException => return "threw"; end;
    return "ok";
  end Bad;
end Probe;

public class Program
  public static method Main()
    Console.WriteLine("ms999 " + Probe.Ok());
    Console.WriteLine("ms1000 " + Probe.Bad());
  end Main;
end Program;
