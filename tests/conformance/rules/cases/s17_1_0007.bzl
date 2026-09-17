//! case: s17_1_0007
//! doc: spec
//! section: 17.1
//! title: range binds tighter than relational in — worked example
//! rule: "`score in 60..100` parses as `score in (60..100)`" — Range tighter
//!   than Relational.
//! cites: 17.1, 17.2, 17.5
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var score := 75;
    var r := score in 60..100;
    Console.WriteLine(r);
  end Main;
end Program;
