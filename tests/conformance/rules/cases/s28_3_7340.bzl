//! case: s28_3_7340
//! doc: spec
//! section: 28.3
//! title: a lambda body assigns the force-unwrapped read of a nullable owned
//!   local into an owned-state local
//! rule: Assigning a borrow to an owned-state LHS, or an owned value to a
//!   borrowed-state LHS, is rejected
//! cites: 28.8.1, 7.9
//! expect: reject
//! diagnostic: OWNED state
//! notes: Pins the #8782 fix: `v!` is an unmarked read of the owned local
//!   `v`, so `u := v!;` is a borrow stored into the owned-state `u`.
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
    var f := lambda(): int => do var u := new Cell(1); var v: Cell? := new Cell(2); u := v!; return 0; end;
    Console.WriteLine(f());
    return 0;
  end Main;
end Program;
