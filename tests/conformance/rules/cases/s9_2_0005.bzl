//! case: s9_2_0005
//! doc: spec
//! section: 9.2
//! title: another auto-hashable record is an admissible component type
//! rule: Admissible component types include another record that is itself
//!   auto-hashable, applied recursively.
//! expect: run
//! stdout: True
module Conf;

public record Point(X: int, Y: int);

public record Segment
  public readonly property Start: Point;
  public readonly property End: Point;

  public constructor(start: Point, finish: Point)
    Start := start;
    End := finish;
  end constructor;
end Segment;

public class Program
  public static method Main()
    var a := new Segment(new Point(0, 0), new Point(1, 1));
    var b := new Segment(new Point(0, 0), new Point(1, 1));
    Console.WriteLine(a = b);
  end Main;
end Program;
