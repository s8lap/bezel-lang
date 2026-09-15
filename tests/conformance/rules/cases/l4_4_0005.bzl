//! case: l4_4_0005
//! doc: stdlib
//! section: 4.4
//! title: IndexOf raises when startIndex exceeds Count
//! rule: 0 <= startIndex <= Count required, IndexOutOfRangeException
//!   otherwise.
//! cites: 4.4
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    using var arr: Array<int> := [1, 2, 3];
    try
      _ := arr.IndexOf(1, 4);
    catch
      IndexOutOfRangeException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
