//! case: l3_1_0059
//! doc: stdlib
//! section: 3.1
//! title: StringBuilder.Insert with an index equal to the builder's Length is admitted
//! rule: stdlib §3.1's Contracts paragraph: "`Insert` and `Remove` raise
//!   `IndexOutOfRangeException` when the index or the `(startIndex, length)`
//!   window falls outside `0..Length`". The bound `0..Length` is INCLUSIVE at
//!   both ends and §4.6 counts a length in code points - "`s.Length` returns
//!   the number of code points in `s`" - so an index equal to the builder's
//!   code-point length is inside the bound and the call is admitted rather
//!   than raising: the admitting twin of l3_1_0058, which differs by the
//!   index alone.
//! cites: 3.1, 4.6
//! expect: run
//! stdout: inserted
//! issue: 7221
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder("\u00E9");
    try
      _ := sb.Insert(1, "X");
      Console.WriteLine("inserted");
    catch
      IndexOutOfRangeException => Console.WriteLine("raised")
    end;
  end Main;
end Program;
