//! case: s9_4_0020
//! doc: spec
//! section: 9.4
//! title: an element store into a local copy of a record's inline-array field is legal
//! rule: The rule keys on **where the store would land** — never on the chain's
//!   root or the statement's form —
//! cites: 15.1.2
//! expect: run
//! stdout: 1 0
//! notes: twin of s9_4_0012 (one legal edit: the element store targets a local
//!   copy of the field, not `this._cells`); the record's own storage stays 0.
module Conf;

public record Grid
  private _cells: [3]int;

  public constructor()
    _cells := [0, 0, 0];
  end constructor;

  public method Mark(i: int): int
    var c := this._cells;
    c[i] := 1;
    return c[i];
  end Mark;

  public method At(i: int): int => this._cells[i];
end Grid;

public class Program
  public static method Main()
    var g := new Grid();
    var marked := g.Mark(1);
    Console.WriteLine($"{marked} {g.At(1)}");
  end Main;
end Program;
