//! case: s28_3_8_7532
//! doc: spec
//! section: 28.3.8
//! title: a returned closure over a borrow alias reassigned in one branch to an owned local is rejected
//! rule: a captured borrowed local or parameter
//! cites: 28.3.8
//! expect: reject
//! diagnostic: returned closure captures
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
  method G(w: Cell, k: int): () -> int
    var t := new Cell(8);
    var b := w;
    if k >= 0 then b := t; end;
    return lambda() => b.v;
  end G;
  public method Run(i: int): int var c := new Cell(3); var f := G(c, i); return f(); end Run;
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
