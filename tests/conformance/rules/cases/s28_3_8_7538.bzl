//! case: s28_3_8_7538
//! doc: spec
//! section: 28.3.8
//! title: a closure capturing a borrow alias assigned to an outer-block local is rejected
//! rule: whose scope does not strictly enclose any source's scope
//! cites: 28.3.8
//! expect: reject
//! diagnostic: which outlives it
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
  public method Run(i: int): int
    var g: () -> int := lambda() => 0;
    do
      var t := new Cell(8); var b := t;
      var f := lambda() => b.v;
      g := f;
    end;
    return g();
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
