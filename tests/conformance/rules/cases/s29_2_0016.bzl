//! case: s29_2_0016
//! doc: spec
//! section: 29.2
//! title: a member store through a span loop's value-typed iteration variable never reaches the slot
//! rule: For value-typed T the iteration variable is a copy of the slot's value, as
//!   an inline array's is: it carries the borrows the slot's value carries ... and
//!   a member store through it (§9.7's first bullet) lands in the variable, never
//!   in the slot
//! cites: 29.2, 9.7
//! expect: run
//! stdout: 1
module Conf;


public record Cell
  public property V: int;

  public constructor(v: int)
    V := v;
  end constructor;
end Cell;

public class Program
  public static method Main()
    var xs: Array<Cell> := [new Cell(1), new Cell(2)];
    for var c in xs.AsSpan() do
      c.V := 9;
    end;
    Console.WriteLine(xs[0].V.ToString());
  end Main;
end Program;
