//! case: l2_3_0024
//! doc: stdlib
//! section: 2.3
//! title: Convert.TryToDouble returns false exactly where Convert.ToDouble raises
//! rule: `Try` forms return `false` in exactly the cases the throwing forms would
//!   raise; the `out` parameter receives the default value.
//! cites: 2.3, 9.2
//! expect: run
//! issue: 7324
//! stdout: False

module Conf;

public class Program
  public static method Main()
    var ok := Convert.TryToDouble("abc", out var v);
    Console.WriteLine(ok);
  end Main;
end Program;
