//! case: s28_3_8_7534
//! doc: spec
//! section: 28.3.8
//! title: a returned closure local bound from another closure local that captured a borrow alias is rejected
//! rule: may not be returned from the enclosing method
//! cites: 28.3.8
//! expect: reject
//! diagnostic: returned
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
    var t := new Cell(8); var b := t;
    var f := lambda() => b.v;
    var g := f;
    return g;
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
