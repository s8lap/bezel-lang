//! case: l2_5_1203
//! doc: stdlib
//! section: 2.5
//! title:Array of DateTime Sort orders by the full 64-bit tick count
//! rule:CompareTo = tick order (Kind not
//! cites: 4.4
//! expect: run
//! output: 0
//!   1
//!   4294967296
//!   4294967297
//!   8589934593
//!   3155378975999999999
module Conf;

public class Program
  public static method Main()
    var a: Array<DateTime> := [DateTime.FromTicks(8589934593L), DateTime.FromTicks(4294967297L), DateTime.FromTicks(1L), DateTime.FromTicks(3155378975999999999L), DateTime.FromTicks(0L), DateTime.FromTicks(4294967296L)];
    a.Sort();
    for var x in a do
      Console.WriteLine(x.Ticks);
    end for;
  end Main;
end Program;
