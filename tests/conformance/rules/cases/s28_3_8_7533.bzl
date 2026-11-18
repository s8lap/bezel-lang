//! case: s28_3_8_7533
//! doc: spec
//! section: 28.3.8
//! title: a returned closure over a borrow alias of an owned parameter is rejected
//! rule: may not be returned from the enclosing method
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
  method G(owned c: Cell): () -> int
    var b := c;
    return lambda() => b.v;
  end G;
  public method Run(i: int): int var f := G(new Cell(3)); return f(); end Run;
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
