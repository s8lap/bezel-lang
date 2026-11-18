//! case: s28_3_7334
//! doc: spec
//! section: 28.3
//! title: a lambda-local nullable owned binding takes null and then a fresh
//!   producer
//! rule: an **owned-state** LHS requires an owned RHS — a fresh producer, or
//!   a named owned binding under the explicit transfer marker (`x := move
//!   y;`)
//! cites: 28.8.1, 7.9
//! expect: run
//! stdout: 8
//! notes: Pins the #8782 fix against over-rejection: `u: Cell?` takes `null`
//!   and then `new Cell(8)`, neither of which is a borrow of an owned
//!   binding.
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
    var f := lambda(): int => do var u: Cell? := new Cell(1); u := null; u := new Cell(8); return u!.value; end;
    Console.WriteLine(f());
    return 0;
  end Main;
end Program;
