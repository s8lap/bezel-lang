//! case: s17_1_0008
//! doc: spec
//! section: 17.1
//! title: range binds tighter than equality — worked example
//! rule: "`r = 0..10` parses as `r = (0..10)`" — Range tighter than Equality.
//! cites: 17.1, 17.2
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var r := 0..10;
    var eq := r = 0..10;
    Console.WriteLine(eq);
  end Main;
end Program;
