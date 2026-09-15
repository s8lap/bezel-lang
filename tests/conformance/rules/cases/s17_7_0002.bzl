//! case: s17_7_0002
//! doc: spec
//! section: 17.7
//! title: new on a record produces a value — copies are independent
//! rule: "For records, `new` produces a value."
//! cites: 17.7, 3.2
//! expect: run
//! stdout: 1
module Conf;

public record Point
  public property X: int;
  public property Y: int;

  public constructor(x: int, y: int)
    X := x;
    Y := y;
  end constructor;
end Point;

public class Program
  public static method Main()
    var a := new Point(1, 2);
    var b := a;
    b.X := 99;
    Console.WriteLine(a.X);
  end Main;
end Program;
