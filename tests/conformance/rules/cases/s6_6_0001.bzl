//! case: s6_6_0001
//! doc: spec
//! section: 6.6
//! title: a compound assignment on a narrower integer target writes back through a checked narrowing
//! rule: When T is an integer type or char and R is an integer type, the
//!   result is converted back to T by the corresponding §5.2 checked
//!   narrowing: b += 10 on b: byte is b := byte(b + 10).
//! cites: 6.6
//! expect: run
//! stdout: 254
module Conf;

public class Program
  public static method Main()
    var b: byte := 250;
    b += 4;
    Console.WriteLine(b);
  end Main;
end Program;
