//! case: l9_2_0010
//! doc: stdlib
//! section: 9.2
//! title: double.ParseOrNull returns null exactly where Parse raises
//! rule: null exactly where Parse(s) raises; otherwise the parsed value
//! cites: 9.2
//! expect: run
//! issue: 7318
//! stdout: null

module Conf;

public class Program
  public static method Main()
    var n := double.ParseOrNull("abc");
    Console.WriteLine($"{n}");
  end Main;
end Program;
