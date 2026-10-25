//! case: l9_6_0032
//! doc: stdlib
//! section: 9.6
//! title: string.Insert with a startIndex equal to the receiver's Length is admitted
//! rule: public pure method Insert(startIndex: int, value: string): string;
//!   — §9.6 declares the member with no contract paragraph of its own, so the
//!   `StringBuilder` twin's Contracts paragraph decides it: "`Insert` and
//!   `Remove` raise `IndexOutOfRangeException` when the index or the
//!   `(startIndex, length)` window falls outside `0..Length`". The bound
//!   `0..Length` is INCLUSIVE at both ends, so a startIndex equal to the
//!   receiver's Length is inside it and the call is admitted rather than
//!   raising — this is the admitting twin of l9_6_0031, which differs by the
//!   index alone.
//! cites: 9.6, 3.1
//! expect: run
//! stdout: inserted
//! issue: 7172
module Conf;

public class Program
  public static method Main()
    try
      _ := "ab".Insert(2, "z");
      Console.WriteLine("inserted");
    catch
      IndexOutOfRangeException => Console.WriteLine("raised")
    end;
  end Main;
end Program;
