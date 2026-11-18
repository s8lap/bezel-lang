//! case: s28_3_8_7530
//! doc: spec
//! section: 28.3.8
//! title: a returned closure over a two-hop borrow alias chain of an owned local is rejected
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
  method F(k: int): () -> int
    var t := new Cell(8); var a := t; var b := a;
    return lambda() => b.v;
  end F;
  public method Run(i: int): int var f := F(i); return f(); end Run;
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
