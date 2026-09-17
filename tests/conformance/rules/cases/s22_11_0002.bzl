//! case: s22_11_0002
//! doc: spec
//! section: 22.11
//! title: with on a freshly-constructed record is unaffected by the retirement
//! rule: Record `with` on a freshly-constructed record (`new Point(1, 2)
//!   with(Y := 9)`) is unaffected.
//! cites: 22.11, 9.5
//! expect: run
//! stdout: 9
module Conf;

public record Point(X: int, Y: int);

public class Program
  public static method Main()
    var p := new Point(1, 2) with(Y := 9);
    Console.WriteLine(p.Y);
  end Main;
end Program;
