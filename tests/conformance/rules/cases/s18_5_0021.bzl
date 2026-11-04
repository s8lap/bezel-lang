//! case: s18_5_0021
//! doc: spec
//! section: 18.5
//! title: alignment pads the text null of a null function hole, right and left
//! rule: The alignment applies to the rendered text `null` exactly as to any other rendered
//!   text
//! cites: 18.5, 8.5
//! expect: run
//! output: [  null]
//!   [null  ]
module Conf;

public class Program
  public static method Main()
    var f0: ((int) -> int)? := null;
    Console.WriteLine($"[{f0,6}]");
    Console.WriteLine($"[{f0,-6}]");
  end Main;
end Program;
