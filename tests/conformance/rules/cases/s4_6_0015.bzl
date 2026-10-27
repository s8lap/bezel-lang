//! case: s4_6_0015
//! doc: spec
//! section: 4.6
//! title: a negative slice endpoint is out of range over the string's indices
//! rule: Endpoints follow the standard rules for `Range<int>` over `s`'s indices:
//!   ... Out-of-range endpoints raise `IndexOutOfRangeException`; a string's
//!   indices are zero-based code-point positions, so a negative endpoint is out
//!   of range.
//! cites: 4.6, 16.6
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    var r: Range<int> := -2..2;
    try
      Console.WriteLine("abcdef"[r].Length);
    catch
      var ex: IndexOutOfRangeException => Console.WriteLine("caught");
    end try;
  end Main;
end Program;
