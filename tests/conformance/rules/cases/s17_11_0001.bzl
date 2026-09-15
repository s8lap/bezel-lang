//! case: s17_11_0001
//! doc: spec
//! section: 17.11
//! title: default(int) is the numeric zero value
//! rule: "`default(T)` produces the zero value of type T ... Numeric types:
//!   `0`, `0.0`, `0m`, etc."
//! cites: 17.11
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    var n := default(int);
    Console.WriteLine(n);
  end Main;
end Program;
