//! case: l2_18_4501
//! doc: stdlib
//! section: 2.18
//! title: DateOnly renders yyyy, MM and dd as zero-padded fields at the edge values
//! rule: Each of the seven specifiers renders a fixed-width, zero-padded field of
//!   exactly its own letter count
//! cites: 2.5
//! expect: run
//! output: 0001-01-01
//!   9999-12-31
//!   2024/02/29 29.02.2024
//! issue: 2240
module Conf;

public class C
  public static method Main()
    var a := new DateOnly(1, 1, 1);
    var b := new DateOnly(9999, 12, 31);
    var c := new DateOnly(2024, 2, 29);
    Console.WriteLine(a.ToString("yyyy-MM-dd"));
    Console.WriteLine(b.ToString("yyyy-MM-dd"));
    Console.WriteLine(c.ToString("yyyy/MM/dd dd.MM.yyyy"));
  end Main;
end C;
