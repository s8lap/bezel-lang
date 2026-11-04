//! case: s18_5_0025
//! doc: spec
//! section: 18.5
//! title: alignment pads the text null of a null record and a null Object hole
//! rule: The alignment applies to the rendered text `null` exactly as to any other rendered
//!   text
//! cites: 18.5, 8.5
//! expect: run
//! output: [    null]
//!   [null    ]
module Conf;
public record Pt(X: int, Y: int);

public class Program
  public static method Main()
    var np: Pt? := null;
    var no: Object? := null;
    Console.WriteLine($"[{np,8}]");
    Console.WriteLine($"[{no,-8}]");
  end Main;
end Program;
