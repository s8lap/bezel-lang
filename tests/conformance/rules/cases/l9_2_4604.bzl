//! case: l9_2_4604
//! doc: stdlib
//! section: 9.2
//! title: double.ParseOrNull is 0 for a value that rounds to zero and null for one that overflows
//! rule: A value that rounds to zero or to a subnormal is in range: `double.Parse("1e-400")` is `0`.
//! cites: 9.2
//! expect: run
//! stdout: 0 null
module Conf;

public class Program
  public static method Main()
    var a := double.ParseOrNull("1e-400");
    var b := double.ParseOrNull("1e999");
    Console.WriteLine($"{a} {b}");
  end Main;
end Program;
