//! case: l3_1_0058
//! doc: stdlib
//! section: 3.1
//! title: StringBuilder.Insert raises IndexOutOfRangeException when the index is outside 0..Length
//! rule: stdlib §3.1's Contracts paragraph: "`Insert` and `Remove` raise
//!   `IndexOutOfRangeException` when the index or the `(startIndex, length)`
//!   window falls outside `0..Length`". §4.6 counts a length in code points -
//!   "`s.Length` returns the number of code points in `s`" - so a builder
//!   holding one non-ASCII code point has `Length` 1, and the index 2 lies
//!   outside `0..1`: the call raises rather than inserting.
//! cites: 3.1, 4.6
//! expect: run
//! stdout: caught
//! issue: 7221
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder("\u00E9");
    try
      _ := sb.Insert(2, "X");
    catch
      IndexOutOfRangeException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
