//! case: s28_3_7335
//! doc: spec
//! section: 28.3
//! title: a lambda-local interface-typed owned binding is reassigned with
//!   `move`
//! rule: an **owned-state** LHS requires an owned RHS — a fresh producer, or
//!   a named owned binding under the explicit transfer marker (`x := move
//!   y;`)
//! cites: 28.8.1
//! expect: run
//! stdout: 4
//! notes: Pins the #8782 fix on the admitting side for an interface-typed
//!   owned local: the marker makes the RHS an owned transfer.
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
    var f := lambda(): int => do var u: IShape := new Sq(); var v: IShape := new Sq(); u := move v; return u.Area(); end;
    Console.WriteLine(f());
    return 0;
  end Main;
end Program;
