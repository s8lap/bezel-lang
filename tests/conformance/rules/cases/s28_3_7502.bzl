//! case: s28_3_7502
//! doc: spec
//! section: 28.3
//! title: a getter-local owned binding self-assigned under the move marker is admitted
//! rule: an **owned-state** LHS requires an owned RHS — a fresh producer, or a named owned binding under
//!   the explicit transfer marker (`x := move y;`)
//! cites: 28.3
//! expect: run
//! stdout: True
module Conf;
import System;
class Cell
  public v: int;
  public constructor(x: int) this.v := x; end constructor;
end Cell;
class H
  public property Item: Cell?
    get => do
      var t: Cell? := new Cell(1);
      t := move t;
      return null;
    end;
  end Item;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    Console.WriteLine(h.Item = null);
    return 0;
  end Main;
end Program;
