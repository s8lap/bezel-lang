//! case: s28_3_7332
//! doc: spec
//! section: 28.3
//! title: a lambda body assigns between two borrowed-state locals
//! rule: a **borrowed-state** LHS requires a borrow (or `null`) RHS
//! cites: 28.8.1
//! expect: run
//! stdout: 4
//! notes: Pins the #8782 fix on the admitting side: `b` and `b2` are
//!   borrowed-state locals (initialised from the captured owned `c` and `d`
//!   by plain reads), so `b := b2;` is a borrow into a borrowed-state LHS.
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
    var c := new Cell(3);
    var d := new Cell(4);
    var f := lambda(): int => do var b := c; var b2 := d; b := b2; return b.value; end;
    Console.WriteLine(f());
    return 0;
  end Main;
end Program;
