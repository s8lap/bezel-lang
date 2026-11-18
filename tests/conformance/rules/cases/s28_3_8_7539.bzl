//! case: s28_3_8_7539
//! doc: spec
//! section: 28.3.8
//! title: a returned closure whose captured alias is later reassigned to a parameter still carries the owner
//! rule: over every assignment to it in any body
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
class H
  method G(w: Cell): () -> int
    var t := new Cell(8);
    var b := t;
    var f := lambda() => b.v;
    b := w;
    return f;
  end G;
  public method Run(i: int): int var c := new Cell(3); var f := G(c); return f(); end Run;
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
