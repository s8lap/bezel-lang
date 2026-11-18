//! case: s28_3_7333
//! doc: spec
//! section: 28.3
//! title: a lambda body assigns an owned value to a borrowed-state local
//!   after a borrow-take
//! rule: a **borrowed-state** LHS requires a borrow (or `null`) RHS
//! cites: 28.8.1
//! expect: run
//! stdout: 3
//! notes: Pins the #8782 fix against over-rejection: `b := w;` reads the
//!   owned local `w` unmarked, which is a borrow, so the borrowed-state LHS
//!   `b` receives a borrow. Plain reads of named owned bindings are borrows
//!   (28.3 `var x := expr` bullet).
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
    var f := lambda(): int => do var v := new Cell(2); var b := v; var w := new Cell(3); b := w; return b.value; end;
    Console.WriteLine(f());
    return 0;
  end Main;
end Program;
