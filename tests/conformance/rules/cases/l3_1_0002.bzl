//! case: l3_1_0002
//! doc: stdlib
//! section: 3.1
//! title: Remove raises IndexOutOfRangeException outside 0..Length
//! rule: Remove raises IndexOutOfRangeException when the (startIndex,
//!   length) window falls outside 0..Length.
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
