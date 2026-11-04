//! case: l2_4_0028
//! doc: stdlib
//! section: 2.4
//! title: A string? result may be an interpolation hole, which renders null as the text null
//! rule: A hole expression may have a nullable type; no narrowing is required.
//! cites: 2.4, 18.5
//! expect: run
//! output: [x]
//!   [null]
//! issue: 8096
module Conf;

public class Program
  public static method Main()
    Environment.Set("BZ_H3_HOLE", "x");
    Console.WriteLine($"[{Environment.Get("BZ_H3_HOLE")}]");
    Console.WriteLine($"[{Environment.Get("BZ_H3_HOLE_ABSENT")}]");
  end Main;
end Program;
