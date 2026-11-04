//! case: s9_4_0034
//! doc: spec
//! section: 9.4
//! title: A positional record with a non-defaultable body field set only by a body constructor is rejected
//! rule: a body constructor, of any parameter list, does not assign it for the
//!   primary
//! cites: 9.3, 7.9
//! expect: reject
//! diagnostic: _c
//! issue: 8097
//! notes: twin of s9_4_0033 (pairs the rejected with its accepting form).
module Conf;

public class Cell
  public V: int := 0;
end Cell;

public record R(seed: int)
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
