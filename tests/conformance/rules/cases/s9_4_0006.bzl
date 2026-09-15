//! case: s9_4_0006
//! doc: spec
//! section: 9.4
//! title: a store that hops through a class-typed member leaves the receiver and is legal
//! rule: A chain that hops through a class- or interface-typed member leaves the
//!   receiver: a record's reference-typed fields are borrows, so a store beyond
//!   such a hop lands in the borrowed heap object, not in the receiver, and is
//!   legal.
//! cites: 9.7
//! expect: run
//! stdout: 9
module Conf;

public class Cell
  public property Value: int;

  public constructor(value: int)
    Value := value;
  end constructor;
end Cell;

public record View
  public readonly property Target: Cell;

  public constructor(target: Cell)
    Target := target;
  end constructor;

  public method Poke(v: int)
    Target.Value := v;
  end Poke;
end View;

public class Program
  public static method Main()
    using var c := new Cell(0);
    var view := new View(c);
    view.Poke(9);
    Console.WriteLine(c.Value);
  end Main;
end Program;
