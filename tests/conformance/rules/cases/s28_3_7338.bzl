//! case: s28_3_7338
//! doc: spec
//! section: 28.3
//! title: an async lambda body assigns an unmarked read of another owned
//!   local into an owned-state local
//! rule: Assigning a borrow to an owned-state LHS, or an owned value to a
//!   borrowed-state LHS, is rejected
//! cites: 28.8.1
//! expect: reject
//! diagnostic: OWNED state
//! notes: Pins the #8782 fix for an async lambda body: the same store as
//!   s28_3_7336 inside `async lambda`.
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
  public static async method Main(): Task<int>
    var f := async lambda(): Task<int> => do var u := new Cell(1); var v := new Cell(2); u := v; return 0; end;
    return await f();
  end Main;
end Program;
