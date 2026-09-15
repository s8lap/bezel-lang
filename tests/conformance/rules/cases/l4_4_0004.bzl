//! case: l4_4_0004
//! doc: stdlib
//! section: 4.4
//! title: IndexOf with startIndex equal to Count is legal and returns -1
//! rule: 0 <= startIndex <= Count required, IndexOutOfRangeException
//!   otherwise; startIndex = Count is legal and returns -1.
//! cites: 4.4
//! expect: run
//! stdout: -1
module Conf;

public class Program
  public static method Main()
    using var arr: Array<int> := [1, 2, 3];
    Console.WriteLine(arr.IndexOf(1, arr.Count));
  end Main;
end Program;
