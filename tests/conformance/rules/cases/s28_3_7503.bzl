//! case: s28_3_7503
//! doc: spec
//! section: 28.3
//! title: a lambda-local owned binding self-assigned under the move marker is admitted and still readable
//! rule: an **owned-state** LHS requires an owned RHS — a fresh producer, or a named owned binding under
//!   the explicit transfer marker (`x := move y;`)
//! cites: 28.3
//! expect: run
//! stdout: 1
module Conf;
import System;
class Cell
  public v: int;
  public constructor(x: int) this.v := x; end constructor;
end Cell;
public class Program
  public static method Main(): int
    var f := lambda(): int => do var u := new Cell(1); u := move u; return u.v; end;
    Console.WriteLine(f());
    return 0;
  end Main;
end Program;
