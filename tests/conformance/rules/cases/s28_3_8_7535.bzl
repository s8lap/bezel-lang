//! case: s28_3_8_7535
//! doc: spec
//! section: 28.3.8
//! title: a closure over a borrow alias stored into a field of a class is rejected
//! rule: a function value may not be stored in a field, in any collection, in a property or indexer, or
//!   through an `out` or `ref` parameter
//! cites: 28.3.8
//! expect: reject
//! diagnostic: stored into a field
module Conf;
import System;
import System.Collections.Generic;
type Fn := () -> int;
class Cell
  public v: int;
  public constructor(x: int) this.v := x; end constructor;
end Cell;
record R(A: int);
record RefRec(W: Cell?, N: int);
class H
  private _fn: () -> int := lambda() => 0;
  public method Run(i: int): int
    do
      var t := new Cell(8); var b := t;
      this._fn := lambda() => b.v;
    end;
    return this._fn();
  end Run;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var acc := 0;
    for var i in 0..<200 do acc := acc + h.Run(i); end;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
