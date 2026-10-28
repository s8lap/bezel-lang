//! case: l9_6_0037
//! doc: stdlib
//! section: 9.6
//! title: a window reaching past the end of the string raises IndexOutOfRangeException
//! rule: stdlib §9.6 (v0.345) "The window members: `Substring`, `Remove` and `Insert`":
//!   "a window reaching past the end (`startIndex + length > Length`) raises
//!   `IndexOutOfRangeException` … `"abc".Substring(2, 2)` raises".
//! cites: 9.6
//! expect: run
//! stdout: RANGE
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine("abc".Substring(2, 2));
    catch
      IndexOutOfRangeException => Console.WriteLine("RANGE");
    end;
  end Main;
end Program;
