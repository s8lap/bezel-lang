//! case: s28_3_7504
//! doc: spec
//! section: 28.3
//! title: a borrowed-state local self-assigned in a lambda body is admitted
//! rule: a **borrowed-state** LHS requires a borrow (or `null`) RHS
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
    using var r := new Cell(1);
    var f := lambda(): int => do var b := r; b := b; return b.v; end;
    Console.WriteLine(f());
    return 0;
  end Main;
end Program;
