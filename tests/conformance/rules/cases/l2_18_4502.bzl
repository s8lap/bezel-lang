//! case: l2_18_4502
//! doc: stdlib
//! section: 2.18
//! title: TimeOnly renders HH, mm, ss and fff as zero-padded fields at the edge values
//! rule: Each of the seven specifiers renders a fixed-width, zero-padded field of
//!   exactly its own letter count
//! cites: 2.5
//! expect: run
//! output: 00:00:00.000
//!   23:59:59.999
//!   00:00:00.001
//!   12:00:00.500
//!   59-59-23
//!   [T] at:
//! issue: 2240
module Conf;

public class C
  public static method Main()
    var m := new TimeOnly(0, 0, 0, 0);
    var n := new TimeOnly(23, 59, 59, 999);
    var o := new TimeOnly(0, 0, 0, 1);
    var p := new TimeOnly(12, 0, 0, 500);
    Console.WriteLine(m.ToString("HH:mm:ss.fff"));
    Console.WriteLine(n.ToString("HH:mm:ss.fff"));
    Console.WriteLine(o.ToString("HH:mm:ss.fff"));
    Console.WriteLine(p.ToString("HH:mm:ss.fff"));
    Console.WriteLine(n.ToString("ss-mm-HH"));
    Console.WriteLine(n.ToString("[T] at:"));
  end Main;
end C;
