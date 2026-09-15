//! case: s6_5_0002
//! doc: spec
//! section: 6.5
//! title: bitwise complement of a signed integer equals negation of the value plus one
//! rule: ~x is the bitwise complement; for signed types, ~x = -(x + 1).
//! cites: 6.5
//! expect: run
//! stdout: -6
module Conf;

public class Program
  public static method Main()
    var x: int := 5;
    var r := ~x;
    Console.WriteLine(r);
  end Main;
end Program;
