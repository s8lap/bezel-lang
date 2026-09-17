//! case: s5_2_0006
//! doc: spec
//! section: 5.2
//! title: an explicit conversion from bool to an integer yields 1 and 0
//! rule: An explicit conversion from bool to any integer type is admitted:
//!   int(b) yields 0 for false and 1 for true (total, never fails).
//! cites: 5.2
//! expect: run
//! output: 1
//!   0
module Conf;

public class Program
  public static method Main()
    var yes: bool := true;
    var no: bool := false;
    Console.WriteLine(int(yes));
    Console.WriteLine(int(no));
  end Main;
end Program;
