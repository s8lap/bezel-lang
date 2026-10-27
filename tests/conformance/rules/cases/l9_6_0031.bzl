//! case: l9_6_0031
//! doc: stdlib
//! section: 9.6
//! title: string.Insert with a startIndex past the receiver's Length raises IndexOutOfRangeException
//! rule: public pure method Insert(startIndex: int, value: string): string;
//!   — §9.6 declares the member with no contract paragraph of its own, so the
//!   `StringBuilder` twin's Contracts paragraph decides it: "`Insert` and
//!   `Remove` raise `IndexOutOfRangeException` when the index or the
//!   `(startIndex, length)` window falls outside `0..Length`". A startIndex
//!   past the receiver's Length is outside `0..Length`, so the call raises
//!   rather than inserting at the end.
//! cites: 9.6, 3.1
//! expect: run
//! stdout: caught
//! issue: 7172
module Conf;

public class Program
  public static method Main()
    try
      _ := "ab".Insert(999, "z");
    catch
      IndexOutOfRangeException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
