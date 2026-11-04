//! case: s9_4_0014
//! doc: spec
//! section: 9.4
//! title: a record setter body storing beyond a class-typed hop is legal and mutates the borrowed object
//! rule: a store beyond such a hop lands in the borrowed heap object, not in
//!   the receiver, and is **legal** — the sound **mediating** shape, and the
//!   answer to what a record accessor body can do. … a record may still
//!   declare a block-form property or indexer … — including a setter in the
//!   mediating shape above, every store landing beyond a class- or
//!   interface-typed hop —
//! cites: 9.7, 23.3
//! expect: run
//! stdout: 7
//! notes: twin of s9_4_0013.
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

  public property Level: int
    get => Target.Value;
    set => Target.Value := value;
  end Level;
end View;

public class Program
  public static method Main()
    using var c := new Cell(0);
    var v := new View(c);
    v.Level := 7;
    Console.WriteLine(c.Value);
  end Main;
end Program;
