//! case: l2_5_3601
//! doc: stdlib
//! section: 2.5
//! title: ToUtc of a Utc value keeps its ticks
//! rule: `ToUtc()` of a `Kind = Local` value subtracts the offset in force at that local time; of a `Utc`
//!   or `Unspecified` value it keeps `Ticks` and only sets `Kind = Utc`. `ToLocal()` of a `Kind = Utc` or
//!   `Unspecified` value adds the offset in force at that instant; of a `Local` value it returns the value
//!   unchanged.
//! cites: 2.5
//! expect: run
//! stdout: True
//! issue: 8062
//! notes: Zone-independent: no shifted value is observed.
module Conf;

public class Program
  public static method Main()
    var t := 637000000000000000L;
    Console.WriteLine(DateTime.FromTicks(t, DateTimeKind.Utc).ToUtc().Ticks = t);
  end Main;
end Program;
