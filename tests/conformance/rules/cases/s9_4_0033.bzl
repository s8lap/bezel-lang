//! case: s9_4_0033
//! doc: spec
//! section: 9.4
//! title: A mediating indexer on a body record stores beyond a class hop and is legal
//! rule: including a setter in the mediating shape above, every store landing beyond
//!   a class- or interface-typed hop
//! cites: 9.2, 9.4
//! expect: run
//! stdout: 21
//! issue: 8089
module Conf;

public class Cell
  public V: int := 0;
end Cell;

public record R
  public readonly property seed: int;
  private _c: Cell;
  public constructor(seed: int, c: Cell)
    this.seed := seed;
    _c := c;
  end constructor;

  public property [i: int]: int
    get => _c.V + i;
    set => _c.V := value - i;
  end;
end R;

public class Program
  public static method Main()
    using var c := new Cell();
    var r := new R(9, c);
    r[3] := 20;
    Console.WriteLine(r[4]);
  end Main;
end Program;
