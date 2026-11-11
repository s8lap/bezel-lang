//! case: s28_3_4513
//! doc: spec
//! section: 28.3
//! title: a bound lazy query only rendered in a hole spends no enumeration right and
//!   still warns
//! rule: a value produced and never used
//! cites: 29.11
//! expect: warn
//! diagnostic: must-use
//! issue: 8392
module Conf;

public class Program
  public static method Main()
    var names: Array<string> := ["a", "bb", "ccc"];
    var q := names.Where(lambda(n) => n.Length > 1);
    Console.WriteLine($"q is {q}");
  end Main;
end Program;
