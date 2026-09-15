//! case: s22_15_0016
//! doc: spec
//! section: 22.15
//! title: a pure body may mutate a record-typed parameter's fields as frame-private state
//! rule: Locals and frame-local mutation: assignment ... where the
//!   target's root is a local variable or by-value parameter of this body
//!   and the access path from that root traverses only value-typed
//!   storage (record fields, inline-array slots).
//! cites: 22.15
//! expect: run
//! stdout: 2
module Conf;

public record Point
  public property X: int;
  public property Y: int;

  public constructor(x: int, y: int)
    X := x;
    Y := y;
  end constructor;
end Point;

public class Ops
  public static pure method Shift(p: Point): int
    p.X := p.X + 1;
    return p.X;
  end Shift;
end Ops;

public class Program
  public static method Main()
    var pt := new Point(1, 2);
    Console.WriteLine(Ops.Shift(pt));
  end Main;
end Program;
