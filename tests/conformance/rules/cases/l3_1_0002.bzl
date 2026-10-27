//! case: l3_1_0002
//! doc: stdlib
//! section: 3.1
//! title: Remove raises IndexOutOfRangeException outside 0..Length
//! rule: stdlib §3.1's Contracts paragraph: "`Insert` and `Remove` raise
//!   `IndexOutOfRangeException` when the index or the `(startIndex, length)`
//!   window falls outside `0..Length`". The `(startIndex, length)` window
//!   `(10, 1)` over a builder whose `Length` is 5 falls outside `0..5`, so
//!   the call raises.
//! cites: 3.1
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder("hello");
    try
      _ := sb.Remove(10, 1);
    catch
      IndexOutOfRangeException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
