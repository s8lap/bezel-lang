//! case: l2_5_3602
//! doc: stdlib
//! section: 2.5
//! title: ToUtc of an Unspecified value keeps its ticks and sets Kind to Utc
//! rule: `ToUtc()` of a `Kind = Local` value subtracts the offset in force at that local time; of a `Utc`
//!   or `Unspecified` value it keeps `Ticks` and only sets `Kind = Utc`. `ToLocal()` of a `Kind = Utc` or
//!   `Unspecified` value adds the offset in force at that instant; of a `Local` value it returns the value
//!   unchanged.
//! cites: 2.5
//! expect: run
//! output: True
//!   True
//! issue: 8062
//! notes: Zone-independent: no shifted value is observed.
module Conf;

public class Program
  public static method Main()
    var t := 637000000000000000L;
    var u := DateTime.FromTicks(t).ToUtc();
    Console.WriteLine(u.Ticks = t);
    Console.WriteLine(u.Kind = DateTimeKind.Utc);
  end Main;
end Program;
