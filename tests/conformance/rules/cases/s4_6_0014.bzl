//! case: s4_6_0014
//! doc: spec
//! section: 4.6
//! title: an out-of-range slice endpoint raises IndexOutOfRangeException
//! rule: Endpoints follow the standard rules for `Range<int>` over `s`'s indices:
//!   ... Out-of-range endpoints raise `IndexOutOfRangeException`.
//! cites: 4.6
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    var r: Range<int> := 0..99;
    try
      Console.WriteLine("abcdef"[r].Length);
    catch
      var ex: IndexOutOfRangeException => Console.WriteLine("caught");
    end try;
  end Main;
end Program;
