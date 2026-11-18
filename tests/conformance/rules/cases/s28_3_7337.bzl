//! case: s28_3_7337
//! doc: spec
//! section: 28.3
//! title: a nested lambda assigns an unmarked read of another owned local
//!   into an owned-state local
//! rule: Assigning a borrow to an owned-state LHS, or an owned value to a
//!   borrowed-state LHS, is rejected
//! cites: 28.8.1
//! expect: reject
//! diagnostic: OWNED state
//! notes: Pins the #8782 fix through nesting: the rejected store sits in the
//!   inner lambda, whose locals `u` and `v` are both its own.
module Conf;

public class R: Exception
  public constructor() end constructor;
end R;

public class Cell
  public value: int := 0;
  public constructor(v: int) this.value := v; end constructor;
end Cell;

public interface IShape method Area(): int; end IShape;

public class Sq: IShape
  public constructor() end constructor;
  public method Area(): int => 4;
end Sq;

public class Program
  public static method Main(): int
    var f := lambda(): int => do
      var g := lambda(): int => do var u := new Cell(1); var v := new Cell(2); u := v; return 0; end;
      return g();
    end;
    Console.WriteLine(f());
    return 0;
  end Main;
end Program;
