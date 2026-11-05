//! case: l2_5_1205
//! doc: stdlib
//! section: 2.5
//! title:Array of TimeSpan Sorted orders negative, zero and large tick counts by signed tick order
//! rule:CompareTo = tick order. Ordering is CompareTo only
//! cites: 4.4
//! expect: run
//! output: -9223372036854775808
//!   -8589934592
//!   -1
//!   0
//!   1
//!   4294967296
//!   9223372036854775807
module Conf;

public class Program
  public static method Main()
    var a: Array<TimeSpan> := [TimeSpan.MaxValue, TimeSpan.FromTicks(-8589934592L), TimeSpan.MinValue, TimeSpan.Zero, TimeSpan.FromTicks(4294967296L), TimeSpan.FromTicks(-1L), TimeSpan.FromTicks(1L)];
    var s := a.Sorted();
    for var x in s do
      Console.WriteLine(x.Ticks);
    end for;
  end Main;
end Program;
