//! case: l9_1_4604
//! doc: stdlib
//! section: 9.1
//! title: int.ParseOrNull admits a leading plus and a negative zero
//! rule: an optional leading `+` or `-`, then one or more digits of that radix, and nothing else
//! cites: 9.1, 1
//! expect: run
//! stdout: 42 0
module Conf;

public class Program
  public static method Main()
    var a := int.ParseOrNull("+42");
    var b := int.ParseOrNull("-0");
    Console.WriteLine($"{a} {b}");
  end Main;
end Program;
