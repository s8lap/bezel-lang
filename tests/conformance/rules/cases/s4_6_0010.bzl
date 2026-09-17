//! case: s4_6_0010
//! doc: spec
//! section: 4.6
//! title: interpolation is the correct way to embed a non-string value
//! rule: The compiler does not implicitly convert other types to string; use
//!   interpolation or .ToString().
//! cites: 4.6, 18.5
//! expect: run
//! stdout: n = 5
module Conf;

public class Program
  public static method Main()
    var count := 5;
    Console.WriteLine($"n = {count}");
  end Main;
end Program;
