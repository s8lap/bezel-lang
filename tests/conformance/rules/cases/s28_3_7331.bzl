//! case: s28_3_7331
//! doc: spec
//! section: 28.3
//! title: a lambda-local owned binding is reassigned from an owned-returning
//!   call
//! rule: an **owned-state** LHS requires an owned RHS — a fresh producer, or
//!   a named owned binding under the explicit transfer marker (`x := move
//!   y;`)
//! cites: 28.8.1
//! expect: run
//! stdout: 9
//! notes: Pins the #8782 fix on the admitting side: `Make()` returns `owned
//!   Cell`, a fresh producer, so `u := Make();` is an owned RHS for the
//!   owned-state LHS.
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

public class Maker
  public static method Make(): owned Cell
    return new Cell(9);
  end Make;
end Maker;

public class Program
  public static method Main(): int
    var f := lambda(): int => do var u := new Cell(1); u := Maker.Make(); return u.value; end;
    Console.WriteLine(f());
    return 0;
  end Main;
end Program;
