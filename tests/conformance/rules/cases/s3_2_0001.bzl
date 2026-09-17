//! case: s3_2_0001
//! doc: spec
//! section: 3.2
//! title: value-typed assignment copies an independent value
//! rule: A value type is copied on assignment; each variable holds an
//!   independent value.
//! cites: 3.2
//! expect: run
//! stdout: 1
module Conf;

public record Point(X: int, Y: int);

public class Program
  public static method Main()
    var p1 := new Point(1, 2);
    var p2 := p1;
    p2 := new Point(3, 4);
    Console.WriteLine($"{p1.X}");
  end Main;
end Program;
