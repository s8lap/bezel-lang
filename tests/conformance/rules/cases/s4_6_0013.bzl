//! case: s4_6_0013
//! doc: spec
//! section: 4.6
//! title: an empty range slice produces the empty string
//! rule: Out-of-range endpoints raise `IndexOutOfRangeException`; an empty range
//!   produces the empty string.
//! cites: 4.6, 16.6
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    var r: Range<int> := 3..1;
    Console.WriteLine("abcdef"[r].Length);
  end Main;
end Program;
