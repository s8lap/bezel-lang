//! case: s20_0001
//! doc: spec
//! section: 20
//! title: the if expression selects the branch of the first true condition
//! rule: "The `if` expression selects between values, condition-first" — example
//!   `var max := (if a > b then a else b);`.
//! cites: 20
//! expect: run
//! stdout: 7
module Conf;

public class Program
  public static method Main()
    var a := 3;
    var b := 7;
    var max := (if a > b then a else b);
    Console.WriteLine(max);
  end Main;
end Program;
