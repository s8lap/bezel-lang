//! case: l2_18_4503
//! doc: stdlib
//! section: 2.18
//! title: an interpolation hole with a time pattern renders the fff field as the
//!   millisecond component
//! rule: Each of the seven specifiers renders a fixed-width, zero-padded field of
//!   exactly its own letter count
//! cites: 2.5
//! expect: run
//! output: 13:05:09.007|09
//! issue: 2240
module Conf;

public class C
  public static method Main()
    var t := new TimeOnly(13, 5, 9, 7);
    Console.WriteLine($"{t:HH:mm:ss.fff}|{t:ss}");
  end Main;
end C;
